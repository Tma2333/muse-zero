/* Muse Bridge IR receive: portable ring + accounting. The platform
 * callback copies decoded values in; the executor thread drains. */
#include "module_ir.h"

#include <string.h>

static MbIrHal ir_hal;
static bool ir_hal_bound;
static MbIrState* ir_active; /* current job's state, executor-owned */
static MbIrSummary ir_summary;

void mb_ir_last_summary(MbIrSummary* out) {
    if(out != NULL) *out = ir_summary;
}

void mb_ir_module_bind(const MbIrHal* hal) {
    if(hal != NULL) ir_hal = *hal;
    ir_hal_bound = hal != NULL;
}

static void (*ir_received_hook)(void* ctx);
static void* ir_received_hook_ctx;

void mb_ir_set_received_hook(void (*fn)(void* ctx), void* ctx) {
    ir_received_hook = fn;
    ir_received_hook_ctx = ctx;
}

static void ir_fire_received(void) {
    if(ir_received_hook != NULL) ir_received_hook(ir_received_hook_ctx);
}

bool mb_ir_validate_params(const MbIrParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->protocol_filter != MB_IR_PROTOCOL_NEC ||
       params->timeout_ms == 0 || params->timeout_ms > MB_IR_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

void mb_ir_on_decoded(uint8_t protocol_id, uint32_t address, uint32_t command, bool repeat) {
    MbIrState* s = ir_active;
    if(s == NULL) return;
    if(protocol_id != s->params.protocol_filter) return; /* unqualified: not counted */
    s->decoded++;
    uint32_t head = s->r_head;
    if(head - s->r_tail >= MB_IR_RING) {
        s->ring_dropped++;
        return;
    }
    MbIrEvent* e = &s->ring[head % MB_IR_RING];
    e->seq = ++s->next_seq;
    e->protocol = (uint8_t)protocol_id;
    e->repeat = repeat ? 1 : 0;
    e->address = address;
    e->command = command;
    s->r_head = head + 1; /* publish after the slot is written */
}

static MbStatus ir_validate_fn(const void* params) {
    MbStatus s;
    if(!ir_hal_bound) return MB_INVALID_ARGUMENT;
    return mb_ir_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus ir_start(MbJobContext* job, const void* params) {
    MbIrState* s = job->module_state;
    const MbIrParams* p = params;
    if(s == NULL || p == NULL || !ir_hal_bound) return MB_INIT_FAILED;
    s->params = *p;
    ir_active = s;
    if(!ir_hal.rx_start(ir_hal.ctx)) {
        ir_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void ir_service(MbJobContext* job, uint32_t now) {
    MbIrState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    /* Drain the decode ring into the job data pool. Record bytes:
     * {event_seq:u32, protocol:u8, repeat:u8, address:u32,
     * command:u32} little-endian (14 bytes). */
    while(s->r_tail != s->r_head) {
        MbIrEvent* e = &s->ring[s->r_tail % MB_IR_RING];
        uint8_t rec[14];
        memcpy(rec + 0, &e->seq, 4);
        rec[4] = e->protocol;
        rec[5] = e->repeat;
        memcpy(rec + 6, &e->address, 4);
        memcpy(rec + 10, &e->command, 4);
        s->r_tail++;
        if(job->emit(job, rec, sizeof(rec))) {
            s->emitted++;
        } else {
            /* pool full: executor counts its own drops; the hole is
             * visible to the host through event_seq */
        }
    }
    if(s->decoded > 0 && !s->rx_signaled) {
        s->rx_signaled = true;
        ir_fire_received(); /* v2: beep = first frame landed */
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_OK;
    }
}

static void ir_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbIrState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult ir_cleanup(MbJobContext* job) {
    MbIrState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    /* Stop + join + free the worker before releasing anything; after
     * this returns, no callback can arrive for this job. */
    if(ir_hal_bound && s->worker_on) {
        ir_hal.rx_stop(ir_hal.ctx);
        s->worker_on = false;
    }
    s->stranded = s->r_head - s->r_tail;
    ir_summary.decoded = s->decoded;
    ir_summary.emitted = s->emitted;
    ir_summary.dropped = s->decoded - s->emitted;
    ir_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_ir = {
    .validate = ir_validate_fn,
    .ctx_size = sizeof(MbIrState),
    .start = ir_start,
    .service = ir_service,
    .request_stop = ir_request_stop,
    .cleanup = ir_cleanup,
};

/* ---- C13 finite TX: portable provider + lifecycle ---- */
static MbIrTxState* ir_tx_active;
static MbIrTxSummary ir_tx_summary;

void mb_ir_tx_last_summary(MbIrTxSummary* out) {
    if(out != NULL) *out = ir_tx_summary;
}

bool mb_ir_tx_validate_params(const MbIrTxParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->protocol != MB_IR_PROTOCOL_NEC ||
       params->frame_count != 1 || params->address > 0xFF || params->command > 0xFF ||
       params->timeout_ms == 0 || params->timeout_ms > MB_IR_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

MbIrTxSupply mb_ir_tx_supply(uint8_t* protocol, uint32_t* address, uint32_t* command) {
    MbIrTxState* s = ir_tx_active;
    if(s == NULL || s->supplied >= s->params.frame_count) {
        if(s != NULL) s->exhausted = true;
        return MB_IR_TX_STOP;
    }
    if(protocol != NULL) *protocol = (uint8_t)s->params.protocol;
    if(address != NULL) *address = s->params.address;
    if(command != NULL) *command = s->params.command;
    s->supplied++;
    return MB_IR_TX_NEW;
}

void mb_ir_tx_on_sent(void) {
    MbIrTxState* s = ir_tx_active;
    if(s != NULL) s->sent++;
}

static MbStatus ir_tx_validate_fn(const void* params) {
    MbStatus s;
    if(!ir_hal_bound || ir_hal.tx_start == NULL || ir_hal.tx_stop == NULL) {
        return MB_INVALID_ARGUMENT;
    }
    return mb_ir_tx_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus ir_tx_start(MbJobContext* job, const void* params) {
    MbIrTxState* s = job->module_state;
    const MbIrTxParams* p = params;
    if(s == NULL || p == NULL || !ir_hal_bound) return MB_INIT_FAILED;
    s->params = *p;
    ir_tx_active = s;
    if(!ir_hal.tx_start(ir_hal.ctx)) {
        ir_tx_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void ir_tx_service(MbJobContext* job, uint32_t now) {
    MbIrTxState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    /* Completion = provider exhaustion: the frame is fully encoded
     * into the worker's stream. (The message-sent callback never
     * fires for a lone frame — see module_ir.h — so it cannot be the
     * completion trigger. Physical completion is established by the
     * quiescing TX stop in cleanup, before the terminal is recorded.)
     * The timeout guards a worker that never finishes asking. */
    if(s->exhausted) {
        s->finished = true;
        job->done = true;
        job->done_status = MB_OK;
        return;
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_TIMEOUT;
    }
}

static void ir_tx_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbIrTxState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult ir_tx_cleanup(MbJobContext* job) {
    MbIrTxState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    /* The platform TX stop waits out any in-flight signal before the
     * worker is freed, so after this returns the emitter is quiescent
     * and only then is the terminal state recorded. */
    bool joined = false;
    if(ir_hal_bound && s->worker_on) {
        ir_hal.tx_stop(ir_hal.ctx);
        s->worker_on = false;
        joined = true;
    }
    ir_tx_summary.supplied = s->supplied;
    ir_tx_summary.sent = s->sent;
    if(s->finished && joined && ir_tx_summary.sent < ir_tx_summary.supplied) {
        /* The job finished via exhaustion and the TX stop above has
         * waited out the in-flight signal and joined the worker: the
         * supplied frames have physically completed. (On timeout or
         * stop paths `finished` is false and the raw callback count
         * stands.) */
        ir_tx_summary.sent = ir_tx_summary.supplied;
    }
    ir_tx_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_ir_tx = {
    .validate = ir_tx_validate_fn,
    .ctx_size = sizeof(MbIrTxState),
    .start = ir_tx_start,
    .service = ir_tx_service,
    .request_stop = ir_tx_request_stop,
    .cleanup = ir_tx_cleanup,
};

/* ---- C15 raw capture ---- */
static MbIrRawRxState* ir_raw_active;
static MbIrRawSummary ir_raw_summary;
static uint32_t ir_raw_stage_snap[MB_IR_RAW_STAGE_MAX];

bool mb_ir_raw_validate_params(const MbIrRawParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->timeout_ms == 0 || params->timeout_ms > MB_IR_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

void mb_ir_raw_last_summary(MbIrRawSummary* out) {
    if(out != NULL) *out = ir_raw_summary;
}

void mb_ir_raw_on_timings(const uint32_t* timings, size_t count) {
    MbIrRawRxState* s = ir_raw_active;
    if(s == NULL || s->have || timings == NULL || count == 0) return;
    size_t keep = count > MB_IR_RAW_STAGE_MAX ? MB_IR_RAW_STAGE_MAX : count;
    memcpy(s->stage, timings, keep * sizeof(uint32_t));
    s->count = (uint32_t)count; /* full worker count; keep is staged */
    s->have = true; /* publish after the copy */
}

static MbStatus ir_raw_validate_fn(const void* params) {
    MbStatus s;
    if(!ir_hal_bound || ir_hal.rx_start_raw == NULL) return MB_INVALID_ARGUMENT;
    return mb_ir_raw_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus ir_raw_start(MbJobContext* job, const void* params) {
    MbIrRawRxState* s = job->module_state;
    const MbIrRawParams* p = params;
    if(s == NULL || p == NULL || !ir_hal_bound) return MB_INIT_FAILED;
    s->params = *p;
    ir_raw_active = s;
    if(!ir_hal.rx_start_raw(ir_hal.ctx)) {
        ir_raw_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void ir_raw_service(MbJobContext* job, uint32_t now) {
    MbIrRawRxState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    if(s->have) {
        /* v2: beep only for a usable capture; over-cap ends in the
         * app-side error flash instead. */
        if(s->count <= MB_IR_OBJ_TIMING_CAP && !s->rx_signaled) {
            s->rx_signaled = true;
            ir_fire_received();
        }
        job->done = true;
        /* Over-cap is an explicit terminal, never a silent truncate
         * (plan section 8); within-cap captures complete OK and the
         * store publish decides completeness of durations. */
        job->done_status = s->count > MB_IR_OBJ_TIMING_CAP ? MB_OVERFLOW : MB_OK;
        return;
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_OK; /* no capture: an honest empty result */
    }
}

static void ir_raw_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbIrRawRxState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult ir_raw_cleanup(MbJobContext* job) {
    MbIrRawRxState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    if(ir_hal_bound && s->worker_on) {
        ir_hal.rx_stop(ir_hal.ctx);
        s->worker_on = false;
    }
    ir_raw_summary.have = s->have;
    ir_raw_summary.count = s->have ? s->count : 0;
    ir_raw_summary.stored = 0;
    ir_raw_summary.timings = NULL;
    if(s->have) {
        uint32_t keep = s->count > MB_IR_RAW_STAGE_MAX ? MB_IR_RAW_STAGE_MAX : s->count;
        memcpy(ir_raw_stage_snap, s->stage, keep * sizeof(uint32_t));
        ir_raw_summary.stored = keep;
        ir_raw_summary.timings = ir_raw_stage_snap;
        ir_raw_summary.captures_total++;
        if(s->count > MB_IR_OBJ_TIMING_CAP) ir_raw_summary.over_cap_total++;
    }
    ir_raw_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_ir_raw_rx = {
    .validate = ir_raw_validate_fn,
    .ctx_size = sizeof(MbIrRawRxState),
    .start = ir_raw_start,
    .service = ir_raw_service,
    .request_stop = ir_raw_request_stop,
    .cleanup = ir_raw_cleanup,
};

/* ---- C15 finite raw transmit ---- */
static MbIrTxRawBind ir_tx_raw_bind;
static bool ir_tx_raw_bound;
static MbIrTxRawState* ir_tx_raw_active;
static MbIrTxRawSummary ir_tx_raw_summary;
static uint32_t ir_tx_raw_stage_buf[MB_IR_OBJ_TIMING_CAP];
static uint16_t ir_tx_raw_stage_count;
static bool ir_tx_raw_stage_valid;

void mb_ir_tx_raw_bind(const MbIrTxRawBind* bind) {
    if(bind != NULL) ir_tx_raw_bind = *bind;
    ir_tx_raw_bound = bind != NULL;
}

void mb_ir_tx_raw_last_summary(MbIrTxRawSummary* out) {
    if(out != NULL) *out = ir_tx_raw_summary;
}

bool mb_ir_tx_raw_validate_params(const MbIrTxRawParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->frame_count != 1 || params->timeout_ms == 0 ||
       params->timeout_ms > MB_IR_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

void mb_ir_tx_raw_stage(const uint32_t* timings, uint16_t count) {
    if(timings == NULL || count == 0 || count > MB_IR_OBJ_TIMING_CAP) {
        ir_tx_raw_stage_valid = false;
        return;
    }
    memcpy(ir_tx_raw_stage_buf, timings, (size_t)count * sizeof(uint32_t));
    ir_tx_raw_stage_count = count;
    ir_tx_raw_stage_valid = true;
}

MbIrTxSupply mb_ir_tx_raw_supply(const uint32_t** timings, uint16_t* count) {
    MbIrTxRawState* s = ir_tx_raw_active;
    if(s == NULL || s->supplied >= 1) {
        if(s != NULL) s->exhausted = true;
        return MB_IR_TX_STOP;
    }
    if(timings != NULL) *timings = s->timings;
    if(count != NULL) *count = s->count;
    s->supplied++;
    return MB_IR_TX_NEW;
}

void mb_ir_tx_raw_on_sent(void) {
    MbIrTxRawState* s = ir_tx_raw_active;
    if(s != NULL) s->sent++;
}

static MbStatus ir_tx_raw_validate_fn(const void* params) {
    MbStatus s;
    if(!ir_hal_bound || ir_hal.tx_start == NULL || ir_hal.tx_stop == NULL) {
        return MB_INVALID_ARGUMENT;
    }
    return mb_ir_tx_raw_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus ir_tx_raw_start(MbJobContext* job, const void* params) {
    MbIrTxRawState* s = job->module_state;
    const MbIrTxRawParams* p = params;
    if(s == NULL || p == NULL || !ir_hal_bound) return MB_INIT_FAILED;
    if(!ir_tx_raw_stage_valid) return MB_INIT_FAILED;
    s->params = *p;
    memcpy(s->timings, ir_tx_raw_stage_buf, (size_t)ir_tx_raw_stage_count * sizeof(uint32_t));
    s->count = ir_tx_raw_stage_count;
    ir_tx_raw_stage_valid = false; /* staged data is single-use */
    ir_tx_raw_active = s;
    if(ir_tx_raw_bound && ir_tx_raw_bind.pin != NULL) {
        if(!ir_tx_raw_bind.pin(ir_tx_raw_bind.ctx, p->object_id)) {
            ir_tx_raw_active = NULL;
            return MB_INIT_FAILED;
        }
        s->pinned = true;
    }
    if(!ir_hal.tx_start(ir_hal.ctx)) {
        if(s->pinned && ir_tx_raw_bind.unpin != NULL) {
            ir_tx_raw_bind.unpin(ir_tx_raw_bind.ctx, p->object_id);
            s->pinned = false;
        }
        ir_tx_raw_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void ir_tx_raw_service(MbJobContext* job, uint32_t now) {
    MbIrTxRawState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    if(s->exhausted) {
        s->finished = true;
        job->done = true;
        job->done_status = MB_OK;
        return;
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_TIMEOUT;
    }
}

static void ir_tx_raw_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbIrTxRawState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult ir_tx_raw_cleanup(MbJobContext* job) {
    MbIrTxRawState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    bool joined = false;
    if(ir_hal_bound && s->worker_on) {
        ir_hal.tx_stop(ir_hal.ctx);
        s->worker_on = false;
        joined = true;
    }
    ir_tx_raw_summary.supplied = s->supplied;
    ir_tx_raw_summary.sent = s->sent;
    if(s->finished && joined && ir_tx_raw_summary.sent < ir_tx_raw_summary.supplied) {
        ir_tx_raw_summary.sent = ir_tx_raw_summary.supplied;
    }
    if(s->finished) ir_tx_raw_summary.tx_total++;
    if(s->pinned && ir_tx_raw_bound && ir_tx_raw_bind.unpin != NULL) {
        ir_tx_raw_bind.unpin(ir_tx_raw_bind.ctx, s->params.object_id);
        s->pinned = false;
    }
    ir_tx_raw_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_ir_tx_raw = {
    .validate = ir_tx_raw_validate_fn,
    .ctx_size = sizeof(MbIrTxRawState),
    .start = ir_tx_raw_start,
    .service = ir_tx_raw_service,
    .request_stop = ir_tx_raw_request_stop,
    .cleanup = ir_tx_raw_cleanup,
};
