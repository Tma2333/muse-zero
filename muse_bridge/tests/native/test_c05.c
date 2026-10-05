/* C05 native tests: executor, one-job slot, fake module, data pool,
 * stop latch, failure-injection unwind (§18 C05 pass evidence).
 * Deterministic manual clock; counting arena allocator fails the Nth
 * allocation on request. */
#define BRIDGE_QUALIFICATION_BUILD 1
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bridge_executor.h"
#include "module_fake.h"

#ifndef NDEBUG
/* asserts are the test mechanism; Makefile must not define NDEBUG */
#endif

/* ---- counting arena allocator (no real heap: ASan stays silent) ---- */
typedef struct {
    uint8_t arena[64 * 1024];
    size_t off;
    long alloc_calls, free_calls, outstanding;
    long fail_at; /* 1-based ordinal to fail; 0 = never */
} CountAlloc;

static void* cnt_alloc(void* ctx, size_t n) {
    CountAlloc* c = ctx;
    c->alloc_calls++;
    if(c->fail_at != 0 && c->alloc_calls == c->fail_at) return NULL;
    n = (n + 15) & ~(size_t)15;
    assert(c->off + n <= sizeof(c->arena));
    void* p = c->arena + c->off;
    c->off += n;
    c->outstanding++;
    return p;
}

static void cnt_free(void* ctx, void* p) {
    CountAlloc* c = ctx;
    if(p == NULL) return;
    c->free_calls++;
    c->outstanding--;
}

static MbAlloc make_alloc(CountAlloc* c) {
    MbAlloc a = {.alloc = cnt_alloc, .free = cnt_free, .ctx = c};
    return a;
}

typedef struct {
    MbExecutor exec;
    CountAlloc cnt;
    MbAlloc alloc;
} Rig;

static void rig_init(Rig* r) {
    memset(r, 0, sizeof(*r));
    r->alloc = make_alloc(&r->cnt);
    mb_executor_init(&r->exec, &r->alloc);
    mb_fake_execution_count_reset();
}

static MbFakeParams fake_params(uint32_t duration, uint32_t interval) {
    MbFakeParams p = {.duration_ticks = duration, .emit_interval_ticks = interval, .emit_size = 8};
    return p;
}

/* run service until terminal latch is pending (bounded steps) */
static uint64_t run_until_terminal(Rig* r, uint32_t* now, uint32_t step) {
    for(int i = 0; i < 200000; i++) {
        uint64_t job_id;
        if(mb_executor_poll_terminal(&r->exec, &job_id, NULL, NULL)) return job_id;
        *now += step;
        mb_executor_service(&r->exec, *now);
    }
    assert(0 && "job never terminated");
    return 0;
}

static void assert_pool_consistent(Rig* r) {
    unsigned queued = 0;
    for(unsigned i = 0; i < MB_DATA_POOL_SLOTS; i++)
        if(r->exec.pool[i].owner != MB_OWNER_FREE) queued++;
    assert(queued == r->exec.q_count);
}

static void test_happy(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(1000, 100);
    assert(mb_executor_admit(&r.exec, 1, &mb_module_fake, &p, now, 5000) == MB_OK);
    /* busy slot rejects a second admit without consuming anything */
    assert(mb_executor_admit(&r.exec, 2, &mb_module_fake, &p, now, 5000) == MB_BUSY);
    MbDataRecord rec;
    uint32_t last_seq = 0, drained = 0;
    for(int i = 0; i < 300; i++) {
        now += 10;
        mb_executor_service(&r.exec, now);
        assert_pool_consistent(&r);
        while(mb_executor_data_poll(&r.exec, &rec)) {
            assert(rec.seq == last_seq + 1);
            last_seq = rec.seq;
            drained++;
        }
    }
    uint64_t job_id;
    MbStatus status;
    MbStopReason reason;
    assert(mb_executor_poll_terminal(&r.exec, &job_id, &status, &reason));
    assert(job_id == 1 && status == MB_OK && reason == MB_STOP_DONE);
    assert(mb_fake_execution_count() == 1);
    assert(r.exec.starts == 1 && r.exec.terminals == 1);
    assert(drained == r.exec.generated && r.exec.dropped == 0);
    assert(r.exec.generated >= 9); /* ~1 record per 100 ticks over 1000 */
    assert(!mb_executor_busy(&r.exec)); /* reusable after ack */
    assert_pool_consistent(&r);
    assert(r.cnt.outstanding == 0);
    puts("PASS happy: one start, one terminal, ordered data, clean unwind");
}

static void test_cancel_before_start(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(1000, 100);
    assert(mb_executor_admit(&r.exec, 7, &mb_module_fake, &p, now, 5000) == MB_OK);
    assert(mb_executor_request_stop(&r.exec, MB_STOP_CANCEL));
    uint64_t job_id;
    MbStatus status;
    MbStopReason reason;
    run_until_terminal(&r, &now, 10);
    assert(mb_executor_poll_terminal(&r.exec, &job_id, &status, &reason) == false); /* consumed by runner */
    assert(r.exec.terminals == 1 && r.exec.starts == 0);
    assert(mb_fake_execution_count() == 0);
    assert(r.cnt.outstanding == 0);
    /* rerun recording the outcome directly */
    rig_init(&r);
    assert(mb_executor_admit(&r.exec, 7, &mb_module_fake, &p, now, 5000) == MB_OK);
    assert(mb_executor_request_stop(&r.exec, MB_STOP_CANCEL));
    for(int i = 0; i < 10 && !r.exec.terminal_pending; i++) mb_executor_service(&r.exec, ++now);
    assert(mb_executor_poll_terminal(&r.exec, &job_id, &status, &reason));
    assert(status == MB_CANCELLED && reason == MB_STOP_CANCEL && job_id == 7);
    puts("PASS cancel-before-start: never started, one CANCELLED terminal");
}

static void test_cancel_during_run(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100000, 10);
    assert(mb_executor_admit(&r.exec, 3, &mb_module_fake, &p, now, 0) == MB_OK);
    for(int i = 0; i < 30; i++) {
        now += 10;
        mb_executor_service(&r.exec, now);
    }
    assert(mb_executor_state(&r.exec) == MB_JOB_RUNNING);
    assert(r.exec.generated > 10);
    assert(mb_executor_request_stop(&r.exec, MB_STOP_LOCAL));
    uint64_t job_id;
    MbStatus status;
    MbStopReason reason;
    for(int i = 0; i < 10 && !r.exec.terminal_pending; i++) mb_executor_service(&r.exec, ++now);
    assert(mb_executor_poll_terminal(&r.exec, &job_id, &status, &reason));
    assert(status == MB_CANCELLED && reason == MB_STOP_LOCAL);
    /* accounting closes: every generated record is enqueued or dropped */
    assert(r.exec.generated == r.exec.enqueued + r.exec.dropped);
    MbDataRecord rec;
    while(mb_executor_data_poll(&r.exec, &rec)) {
    }
    assert(r.exec.consumed == r.exec.enqueued);
    assert(r.exec.terminals == 1 && r.cnt.outstanding == 0);
    /* late stop + extra service change nothing: still exactly one terminal */
    assert(!mb_executor_request_stop(&r.exec, MB_STOP_CANCEL));
    mb_executor_service(&r.exec, now + 100);
    assert(!mb_executor_poll_terminal(&r.exec, NULL, NULL, NULL));
    assert(r.exec.terminals == 1);
    puts("PASS cancel-during-run: partial progress accounted, one terminal");
}

static void test_queue_full_stop(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100000, 1); /* emit every tick, no drain */
    assert(mb_executor_admit(&r.exec, 4, &mb_module_fake, &p, now, 0) == MB_OK);
    for(int i = 0; i < 60; i++) {
        now += 1;
        mb_executor_service(&r.exec, now);
        assert_pool_consistent(&r);
    }
    assert(r.exec.dropped > 0); /* pool exhausted, generator kept counting */
    assert(r.exec.generated == r.exec.enqueued + r.exec.dropped);
    assert(r.exec.last_generated_seq == r.exec.generated);
    uint32_t drops_before = r.exec.dropped;
    assert(mb_executor_request_stop(&r.exec, MB_STOP_CANCEL));
    int steps = 0;
    while(!r.exec.terminal_pending && steps < 5) {
        mb_executor_service(&r.exec, ++now);
        steps++;
    }
    assert(r.exec.terminal_pending); /* stop lands despite the full queue */
    MbStatus status;
    assert(mb_executor_poll_terminal(&r.exec, NULL, &status, NULL));
    assert(status == MB_CANCELLED);
    assert(r.exec.dropped >= drops_before);
    puts("PASS queue-full stop: latch independent of data path");
}

static void test_deadline_not_extended(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100000, 0);
    assert(mb_executor_admit(&r.exec, 5, &mb_module_fake, &p, now, 300) == MB_OK);
    /* simulate heartbeat renewals: nothing but time passing */
    for(int i = 0; i < 40; i++) {
        now += 10;
        mb_executor_service(&r.exec, now);
    }
    assert(now == 400);
    MbStatus status;
    MbStopReason reason;
    assert(mb_executor_poll_terminal(&r.exec, NULL, &status, &reason));
    assert(status == MB_TIMEOUT && reason == MB_STOP_TIMEOUT);
    assert(mb_fake_execution_count() == 1); /* it ran, then hit its deadline */
    puts("PASS deadline: absolute from admit, heartbeat never extends it");
}

static void test_alloc_injection(void) {
    /* boundaries: 1 = module ctx alloc, 2 = fake staging alloc */
    for(long fail_at = 1; fail_at <= 2; fail_at++) {
        Rig r;
        rig_init(&r);
        r.cnt.fail_at = fail_at;
        uint32_t now = 0;
        MbFakeParams p = fake_params(1000, 0);
        assert(mb_executor_admit(&r.exec, 9, &mb_module_fake, &p, now, 0) == MB_OK);
        for(int i = 0; i < 10 && !r.exec.terminal_pending; i++) mb_executor_service(&r.exec, ++now);
        MbStatus status;
        assert(mb_executor_poll_terminal(&r.exec, NULL, &status, NULL));
        assert(status == MB_INIT_FAILED);
        assert(r.exec.starts == 0 && mb_fake_execution_count() == 0);
        assert(r.cnt.outstanding == 0); /* partial startup fully unwound */
        assert(!mb_executor_busy(&r.exec)); /* slot reusable */
        /* and a retry without failure succeeds */
        r.cnt.fail_at = 0;
        assert(mb_executor_admit(&r.exec, 10, &mb_module_fake, &p, now, 0) == MB_OK);
        run_until_terminal(&r, &now, 10);
        assert(mb_fake_execution_count() == 1);
    }
    /* module-internal worker-start failure also unwinds */
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(1000, 0);
    p.fail_worker = true;
    assert(mb_executor_admit(&r.exec, 11, &mb_module_fake, &p, now, 0) == MB_OK);
    for(int i = 0; i < 10 && !r.exec.terminal_pending; i++) mb_executor_service(&r.exec, ++now);
    MbStatus status;
    assert(mb_executor_poll_terminal(&r.exec, NULL, &status, NULL));
    assert(status == MB_INIT_FAILED && r.exec.starts == 0);
    assert(mb_fake_execution_count() == 0 && r.cnt.outstanding == 0);
    puts("PASS alloc injection: every start boundary unwinds, slot reusable");
}

static void test_cleanup_failure_faulted(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100, 0);
    p.fail_cleanup = true;
    assert(mb_executor_admit(&r.exec, 12, &mb_module_fake, &p, now, 0) == MB_OK);
    run_until_terminal(&r, &now, 10);
    MbStatus status;
    /* poll_terminal was consumed by the runner; check recorded state */
    assert(r.exec.terminals == 1);
    assert(mb_executor_state(&r.exec) == MB_JOB_FAULTED);
    assert(mb_executor_busy(&r.exec)); /* ownership retained */
    MbFakeParams ok = fake_params(100, 0);
    assert(mb_executor_admit(&r.exec, 13, &mb_module_fake, &ok, now, 0) == MB_BUSY);
    assert(r.cnt.outstanding == 2); /* ctx + staging retained, not freed */
    mb_executor_deinit(&r.exec); /* app teardown path only */
    assert(r.cnt.outstanding == 1); /* module staging stays module-owned */
    (void)status;
    puts("PASS cleanup failure: FAULTED, slot owned, new work refused");
}

static void test_wrap(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = UINT32_MAX - 200;
    MbFakeParams p = fake_params(500, 100);
    assert(mb_executor_admit(&r.exec, 21, &mb_module_fake, &p, now, 1000) == MB_OK);
    uint64_t job_id;
    MbStatus status;
    job_id = run_until_terminal(&r, &now, 10);
    (void)job_id;
    assert(mb_executor_poll_terminal(&r.exec, NULL, NULL, NULL) == false);
    assert(r.exec.terminals == 1 && mb_fake_execution_count() == 1);
    (void)status;
    puts("PASS wrap: lifecycle arithmetic survives tick rollover");
}

int main(void) {
    test_happy();
    test_cancel_before_start();
    test_cancel_during_run();
    test_queue_full_stop();
    test_deadline_not_extended();
    test_alloc_injection();
    test_cleanup_failure_faulted();
    test_wrap();
    puts("PASS all C05 executor tests");
    return 0;
}
