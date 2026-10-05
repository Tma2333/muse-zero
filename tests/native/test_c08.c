/* C08 native tests: job-targeted cancel, race ordering, first-stop-wins,
 * pre-start cancel, pre-start deadline, faulted-slot ownership
 * (plan section 12 C08, sections 5/9/11.3). Deterministic manual clock. */
#define BRIDGE_QUALIFICATION_BUILD 1
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bridge_executor.h"
#include "module_fake.h"

typedef struct {
    uint8_t arena[64 * 1024];
    size_t off;
    long outstanding;
} CountAlloc;

static void* cnt_alloc(void* ctx, size_t n) {
    CountAlloc* c = ctx;
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
    c->outstanding--;
}

typedef struct {
    MbExecutor exec;
    CountAlloc cnt;
    MbAlloc alloc;
} Rig;

static void rig_init(Rig* r) {
    memset(r, 0, sizeof(*r));
    r->alloc = (MbAlloc){.alloc = cnt_alloc, .free = cnt_free, .ctx = &r->cnt};
    mb_executor_init(&r->exec, &r->alloc);
    mb_fake_execution_count_reset();
}

static MbFakeParams fake_params(uint32_t duration, uint32_t interval) {
    MbFakeParams p = {.duration_ticks = duration, .emit_interval_ticks = interval, .emit_size = 8};
    return p;
}

static void service_n(Rig* r, uint32_t* now, int n, uint32_t step) {
    for(int i = 0; i < n; i++) {
        *now += step;
        mb_executor_service(&r->exec, *now);
    }
}

static void drain_terminal(Rig* r, MbStatus* status, MbStopReason* reason) {
    int guard = 0;
    while(!r->exec.terminal_pending && guard++ < 1000) {
        mb_executor_service(&r->exec, r->exec.admit_tick + (uint32_t)guard * 10u + 100000u);
    }
    assert(r->exec.terminal_pending);
    assert(mb_executor_poll_terminal(&r->exec, NULL, status, reason));
}

static void test_target_mismatch(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(1000, 0);
    assert(mb_executor_admit(&r.exec, 5, &mb_module_fake, &p, now, 0) == MB_OK);
    assert(!mb_executor_request_stop_job(&r.exec, 4, MB_STOP_CANCEL));
    assert(!mb_executor_request_stop_job(&r.exec, 0, MB_STOP_CANCEL));
    assert(!r.exec.stop_latch);
    service_n(&r, &now, 5, 10);
    assert(mb_executor_state(&r.exec) == MB_JOB_RUNNING);
    assert(mb_fake_execution_count() == 1);
    assert(mb_executor_request_stop_job(&r.exec, 5, MB_STOP_CANCEL));
    MbStatus st;
    MbStopReason rs;
    drain_terminal(&r, &st, &rs);
    assert(st == MB_CANCELLED && rs == MB_STOP_CANCEL);
    puts("PASS c08.1 targeted stop: wrong/zero ids inert, right id cancels");
}

static void test_never_cancels_newer(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100, 0);
    assert(mb_executor_admit(&r.exec, 5, &mb_module_fake, &p, now, 0) == MB_OK);
    MbStatus st;
    MbStopReason rs;
    drain_terminal(&r, &st, &rs);
    assert(st == MB_OK);
    /* slot reused by a newer job; a stale cancel for 5 must be inert */
    assert(mb_executor_admit(&r.exec, 6, &mb_module_fake, &p, now, 0) == MB_OK);
    assert(!mb_executor_request_stop_job(&r.exec, 5, MB_STOP_CANCEL));
    assert(!r.exec.stop_latch);
    drain_terminal(&r, &st, &rs);
    assert(st == MB_OK && rs == MB_STOP_DONE);
    assert(mb_fake_execution_count() == 2);
    puts("PASS c08.2 stale cancel never cancels the newer job");
}

static void test_race_orders(void) {
    /* stop latched before the final service: CANCELLED wins */
    {
        Rig r;
        rig_init(&r);
        uint32_t now = 0;
        MbFakeParams p = fake_params(100, 0);
        assert(mb_executor_admit(&r.exec, 1, &mb_module_fake, &p, now, 0) == MB_OK);
        service_n(&r, &now, 5, 10); /* running, 50/100 elapsed */
        assert(mb_executor_state(&r.exec) == MB_JOB_RUNNING);
        assert(mb_executor_request_stop_job(&r.exec, 1, MB_STOP_CANCEL));
        service_n(&r, &now, 20, 10); /* module would have finished inside here */
        MbStatus st;
        MbStopReason rs;
        assert(mb_executor_poll_terminal(&r.exec, NULL, &st, &rs));
        assert(st == MB_CANCELLED && rs == MB_STOP_CANCEL);
        assert(r.exec.terminals == 1);
        assert(!mb_executor_poll_terminal(&r.exec, NULL, NULL, NULL));
    }
    /* completion processed first: OK wins, late stop changes nothing */
    {
        Rig r;
        rig_init(&r);
        uint32_t now = 0;
        MbFakeParams p = fake_params(100, 0);
        assert(mb_executor_admit(&r.exec, 1, &mb_module_fake, &p, now, 0) == MB_OK);
        int guard = 0;
        while(mb_executor_state(&r.exec) != MB_JOB_STOPPING && guard++ < 1000) {
            now += 10;
            mb_executor_service(&r.exec, now);
        }
        assert(mb_executor_state(&r.exec) == MB_JOB_STOPPING);
        mb_executor_request_stop_job(&r.exec, 1, MB_STOP_CANCEL); /* too late */
        MbStatus st;
        MbStopReason rs;
        drain_terminal(&r, &st, &rs);
        assert(st == MB_OK && rs == MB_STOP_DONE);
        assert(r.exec.terminals == 1);
        assert(!mb_executor_request_stop_job(&r.exec, 1, MB_STOP_CANCEL));
        puts("PASS c08.3 race orders: first terminal outcome wins, exactly one terminal");
    }
}

static void test_first_reason_wins(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100000, 0);
    assert(mb_executor_admit(&r.exec, 2, &mb_module_fake, &p, now, 0) == MB_OK);
    service_n(&r, &now, 3, 10);
    assert(mb_executor_request_stop_job(&r.exec, 2, MB_STOP_CANCEL));
    assert(mb_executor_request_stop(&r.exec, MB_STOP_LOCAL)); /* repeat: recorded, no override */
    assert(r.exec.stop_reason == MB_STOP_CANCEL);
    MbStatus st;
    MbStopReason rs;
    drain_terminal(&r, &st, &rs);
    assert(st == MB_CANCELLED && rs == MB_STOP_CANCEL);
    puts("PASS c08.4 repeated cancellation: first stop intent wins");
}

static void test_prestart_variants(void) {
    /* targeted cancel before the first service: never starts */
    {
        Rig r;
        rig_init(&r);
        uint32_t now = 0;
        MbFakeParams p = fake_params(1000, 0);
        assert(mb_executor_admit(&r.exec, 3, &mb_module_fake, &p, now, 5000) == MB_OK);
        assert(mb_executor_request_stop_job(&r.exec, 3, MB_STOP_CANCEL));
        service_n(&r, &now, 5, 10);
        MbStatus st;
        MbStopReason rs;
        assert(mb_executor_poll_terminal(&r.exec, NULL, &st, &rs));
        assert(st == MB_CANCELLED && r.exec.starts == 0 && mb_fake_execution_count() == 0);
    }
    /* deadline expires while still ACCEPTED: never starts, TIMEOUT */
    {
        Rig r;
        rig_init(&r);
        uint32_t now = 1000;
        MbFakeParams p = fake_params(1000, 0);
        assert(mb_executor_admit(&r.exec, 4, &mb_module_fake, &p, now, 100) == MB_OK);
        int guard = 0;
        while(!r.exec.terminal_pending && guard++ < 10) mb_executor_service(&r.exec, now + 200);
        MbStatus st;
        MbStopReason rs;
        assert(mb_executor_poll_terminal(&r.exec, NULL, &st, &rs));
        assert(st == MB_TIMEOUT && rs == MB_STOP_TIMEOUT);
        assert(r.exec.starts == 0 && mb_fake_execution_count() == 0);
    }
    puts("PASS c08.5 pre-start cancel and pre-start deadline: no start, one terminal");
}

static void test_faulted_slot_ownership(void) {
    Rig r;
    rig_init(&r);
    uint32_t now = 0;
    MbFakeParams p = fake_params(100, 0);
    p.fail_cleanup = true;
    assert(mb_executor_admit(&r.exec, 8, &mb_module_fake, &p, now, 0) == MB_OK);
    MbStatus st;
    MbStopReason rs;
    drain_terminal(&r, &st, &rs);
    assert(st == MB_CLEANUP_FAILED);
    assert(mb_executor_state(&r.exec) == MB_JOB_FAULTED);
    assert(!mb_executor_request_stop_job(&r.exec, 8, MB_STOP_CANCEL));
    MbFakeParams ok = fake_params(100, 0);
    assert(mb_executor_admit(&r.exec, 9, &mb_module_fake, &ok, now, 0) == MB_BUSY);
    puts("PASS c08.6 cleanup failure: FAULTED owns the slot, stops and admits refused");
}

int main(void) {
    test_target_mismatch();
    test_never_cancels_newer();
    test_race_orders();
    test_first_reason_wins();
    test_prestart_variants();
    test_faulted_slot_ownership();
    puts("PASS all C08 native tests (6 groups)");
    return 0;
}
