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
