/* Qualification build: the fake module exists so C05-C09 can qualify
 * the execution plumbing; it is never wire-reachable in this phase. */
#define BRIDGE_QUALIFICATION_BUILD 1
#include "module_fake.h"

#include <string.h>

#ifdef BRIDGE_QUALIFICATION_BUILD

typedef struct {
    uint8_t* staging; /* module-owned scratch, freed on start-fail/cleanup */
    uint32_t t0;
    uint32_t next_emit;
    bool anchored;
    bool stop_seen;
    MbFakeParams params; /* copied: admit params may be transient */
} FakeState;

static uint32_t fake_executions;

uint32_t mb_fake_execution_count(void) {
    return fake_executions;
}

void mb_fake_execution_count_reset(void) {
    fake_executions = 0;
}

static MbStatus fake_validate(const void* params) {
    const MbFakeParams* p = params;
    if(p == NULL) return MB_INVALID_ARGUMENT;
    if(p->duration_ticks == 0) return MB_INVALID_ARGUMENT;
    if(p->emit_size > MB_DATA_PAYLOAD) return MB_INVALID_ARGUMENT;
    return MB_OK;
}

static MbStatus fake_start(MbJobContext* job, const void* params) {
    FakeState* s = job->module_state;
    const MbFakeParams* p = params;
    s->params = *p;
    s->staging = job->alloc->alloc(job->alloc->ctx, 64);
    if(s->staging == NULL) return MB_INIT_FAILED;
    memset(s->staging, 0xA5, 64);
    if(p->fail_worker) {
        job->alloc->free(job->alloc->ctx, s->staging);
        s->staging = NULL;
        return MB_INIT_FAILED;
    }
    fake_executions++;
    return MB_OK;
}

static void fake_service(MbJobContext* job, uint32_t now) {
    FakeState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
        s->next_emit = now;
    }
    if(s->params.emit_interval_ticks != 0 && !s->stop_seen &&
       (int32_t)(now - s->next_emit) >= 0) {
        uint8_t rec[MB_DATA_PAYLOAD];
        memset(rec, (int)(job->job_id & 0xFF), sizeof(rec));
        job->emit(job, rec, s->params.emit_size);
        s->next_emit = now + s->params.emit_interval_ticks;
    }
    if((uint32_t)(now - s->t0) >= s->params.duration_ticks) {
        job->done = true;
        job->done_status = MB_OK;
    }
}

static void fake_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    FakeState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult fake_cleanup(MbJobContext* job) {
    FakeState* s = job->module_state;
    if(s->params.fail_cleanup) return MB_CLEAN_FAILED;
    if(s->staging != NULL) {
        job->alloc->free(job->alloc->ctx, s->staging);
        s->staging = NULL;
    }
    return MB_CLEAN_OK;
}

const MbModule mb_module_fake = {
    .validate = fake_validate,
    .ctx_size = sizeof(FakeState),
    .start = fake_start,
    .service = fake_service,
    .request_stop = fake_request_stop,
    .cleanup = fake_cleanup,
};

#endif /* BRIDGE_QUALIFICATION_BUILD */
