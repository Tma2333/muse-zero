/* Muse Bridge notification module: named finite effects only. */
#include "module_notify.h"

#include <string.h>

void mb_notify_init(MbNotifyState* state) {
    memset(state, 0, sizeof(*state));
}

MbStatus mb_notify_validate(uint8_t effect) {
    if(effect < MB_NOTIFY_GREEN_FLASH || effect > MB_NOTIFY_SIGNALS_ON) {
        return MB_INVALID_ARGUMENT;
    }
    return MB_OK;
}

MbStatus mb_notify_apply(MbNotifyState* state, const MbNotifyHal* hal, uint8_t effect) {
    MbStatus v = mb_notify_validate(effect);
    if(v != MB_OK) return v;
    hal->play(hal->ctx, effect);
    state->effects_played++;
    state->last_effect = effect;
    return MB_OK;
}

/* ---- Executor glue. The HAL play call blocks for the finite effect
 * (at most 250 ms) on the executor thread, per plan 10.4. */
static MbNotifyState* notify_bound_state;
static MbNotifyHal notify_bound_hal;
static bool notify_bound;

void mb_notify_module_bind(MbNotifyState* state, const MbNotifyHal* hal) {
    notify_bound_state = state;
    if(hal != NULL) notify_bound_hal = *hal;
    notify_bound = state != NULL && hal != NULL;
}

static MbStatus notify_validate_fn(const void* params) {
    const MbNotifyParams* p = params;
    if(p == NULL || !notify_bound) return MB_INVALID_ARGUMENT;
    return mb_notify_validate(p->effect);
}

static MbStatus notify_start(MbJobContext* job, const void* params) {
    (void)job;
    if(!notify_bound) return MB_INIT_FAILED;
    const MbNotifyParams* p = params;
    return mb_notify_apply(notify_bound_state, &notify_bound_hal, p->effect);
}

static void notify_service(MbJobContext* job, uint32_t now) {
    (void)now;
    job->done = true;
    job->done_status = MB_OK;
}

static void notify_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)job;
    (void)reason; /* short sequence finishes within its bound (10.4) */
}

static MbCleanupResult notify_cleanup(MbJobContext* job) {
    (void)job;
    return MB_CLEAN_OK;
}

const MbModule mb_module_notify = {
    .validate = notify_validate_fn,
    .ctx_size = 0,
    .start = notify_start,
    .service = notify_service,
    .request_stop = notify_request_stop,
    .cleanup = notify_cleanup,
};
