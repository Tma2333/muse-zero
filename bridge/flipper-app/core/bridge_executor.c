#include "bridge_executor.h"

#include <string.h>

static bool executor_emit(MbJobContext* job, const uint8_t* data, uint16_t len) {
    MbExecutor* e = job->executor;
    e->generated++;
    e->last_generated_seq = ++e->next_seq;
    if(len > MB_DATA_PAYLOAD) len = MB_DATA_PAYLOAD;
    for(unsigned i = 0; i < MB_DATA_POOL_SLOTS; i++) {
        if(e->pool[i].owner != MB_OWNER_FREE) continue;
        e->pool[i].owner = MB_OWNER_PRODUCER;
        e->pool[i].rec.seq = e->next_seq;
        e->pool[i].rec.len = len;
        if(len != 0 && data != NULL) memcpy(e->pool[i].rec.bytes, data, len);
        e->pool[i].owner = MB_OWNER_CONSUMER;
        e->queue[(e->q_head + e->q_count) % MB_DATA_POOL_SLOTS] = (uint8_t)i;
        e->q_count++;
        e->enqueued++;
        return true;
    }
    e->dropped++; /* lossy data class (§8): never blocks the producer */
    return false;
}

void mb_executor_init(MbExecutor* e, const MbAlloc* alloc) {
    memset(e, 0, sizeof(*e));
    e->alloc = alloc;
    e->state = MB_JOB_IDLE;
}

void mb_executor_deinit(MbExecutor* e) {
    if(e->ctx.module_state != NULL && e->alloc != NULL) {
        e->alloc->free(e->alloc->ctx, e->ctx.module_state);
        e->ctx.module_state = NULL;
    }
    e->state = MB_JOB_IDLE;
}

bool mb_executor_busy(const MbExecutor* e) {
    return e->state != MB_JOB_IDLE;
}

MbJobState mb_executor_state(const MbExecutor* e) {
    return e->state;
}

MbStatus mb_executor_admit(
    MbExecutor* e,
    uint64_t job_id,
    const MbModule* module,
    const void* params,
    uint32_t now,
    uint32_t deadline_ticks) {
    if(e->state != MB_JOB_IDLE) return MB_BUSY;
    if(module == NULL) return MB_INVALID_ARGUMENT;
    if(module->validate != NULL) {
        MbStatus v = module->validate(params);
        if(v != MB_OK) return v;
    }
    e->module = module;
    e->job_params = params;
    e->job_id = job_id;
    e->admit_tick = now;
    e->deadline_ticks = deadline_ticks;
    e->stop_latch = false;
    e->stop_reason = MB_STOP_NONE;
    e->stop_notified = false;
    e->started = false;
    memset(&e->ctx, 0, sizeof(e->ctx));
    e->ctx.alloc = e->alloc;
    e->ctx.executor = e;
    e->ctx.job_id = job_id;
    e->ctx.emit = executor_emit;
    e->state = MB_JOB_ACCEPTED;
    return MB_OK;
}

bool mb_executor_request_stop(MbExecutor* e, MbStopReason reason) {
    if(e->state == MB_JOB_IDLE || e->state == MB_JOB_TERMINAL || e->state == MB_JOB_FAULTED) {
        return false;
    }
    if(!e->stop_latch) { /* first stop intent wins */
        e->stop_latch = true;
        e->stop_reason = reason;
    }
    return true;
}

bool mb_executor_request_stop_job(MbExecutor* e, uint64_t job_id, MbStopReason reason) {
    if(job_id == 0 || e->job_id != job_id) return false;
    return mb_executor_request_stop(e, reason);
}

static MbStatus stop_status(MbStopReason reason) {
    switch(reason) {
    case MB_STOP_TIMEOUT:
        return MB_TIMEOUT;
    case MB_STOP_LINK:
        return MB_LINK_LOST;
    case MB_STOP_CANCEL:
    case MB_STOP_LOCAL:
        return MB_CANCELLED;
    default:
        return MB_CANCELLED;
    }
}

static void enter_stopping(MbExecutor* e, MbStatus status, MbStopReason reason) {
    e->pending_status = status;
    e->pending_reason = reason;
    e->state = MB_JOB_STOPPING;
    if(e->ctx.module_state != NULL && e->module != NULL && e->module->request_stop != NULL &&
       !e->stop_notified) {
        e->stop_notified = true;
        e->module->request_stop(&e->ctx, reason);
    }
}

/* Execute the §9.3 cleanup sequence. Synchronous module contract for
 * now; a wedged async module surfaces as FAULTED, never forced. */
static void run_cleanup(MbExecutor* e) {
    if(e->started && e->module != NULL && e->module->cleanup != NULL) {
        if(e->module->cleanup(&e->ctx) != MB_CLEAN_OK) {
            e->terminal_job_id = e->job_id;
            e->terminal_status = MB_CLEANUP_FAILED;
            e->terminal_reason = e->pending_reason;
            e->terminal_pending = true;
            e->terminals++;
            e->state = MB_JOB_FAULTED; /* ownership retained; slot unusable */
            return;
        }
    }
    if(e->ctx.module_state != NULL && e->alloc != NULL) {
        e->alloc->free(e->alloc->ctx, e->ctx.module_state);
        e->ctx.module_state = NULL;
    }
    e->terminal_job_id = e->job_id;
    e->terminal_status = e->pending_status;
    e->terminal_reason = e->pending_reason;
    e->terminal_pending = true;
    e->terminals++;
    e->state = MB_JOB_TERMINAL;
}

void mb_executor_service(MbExecutor* e, uint32_t now) {
    switch(e->state) {
    case MB_JOB_ACCEPTED: {
        if(e->stop_latch) { /* cancel before start: never starts (§9.1) */
            enter_stopping(e, stop_status(e->stop_reason), e->stop_reason);
            break;
        }
        if(e->deadline_ticks != 0 && mb_elapsed(now, e->admit_tick, e->deadline_ticks)) {
            enter_stopping(e, MB_TIMEOUT, MB_STOP_TIMEOUT);
            break;
        }
        e->state = MB_JOB_STARTING;
        if(e->module->ctx_size != 0) {
            e->ctx.module_state = e->alloc->alloc(e->alloc->ctx, e->module->ctx_size);
            if(e->ctx.module_state == NULL) {
                enter_stopping(e, MB_INIT_FAILED, MB_STOP_ERROR);
                break;
            }
            memset(e->ctx.module_state, 0, e->module->ctx_size);
        }
        MbStatus s = e->module->start(&e->ctx, e->job_params);
        if(s != MB_OK) {
            /* module start contract: it freed its own partial state */
            enter_stopping(e, s, MB_STOP_ERROR);
            break;
        }
        e->started = true;
        e->starts++;
        e->state = MB_JOB_RUNNING;
        break;
    }
    case MB_JOB_RUNNING: {
        if(e->stop_latch) {
            enter_stopping(e, stop_status(e->stop_reason), e->stop_reason);
            break;
        }
        if(e->deadline_ticks != 0 && mb_elapsed(now, e->admit_tick, e->deadline_ticks)) {
            enter_stopping(e, MB_TIMEOUT, MB_STOP_TIMEOUT);
            break;
        }
        e->module->service(&e->ctx, now);
        if(e->ctx.done) {
            enter_stopping(e, e->ctx.done_status, MB_STOP_DONE);
        }
        break;
    }
    case MB_JOB_STOPPING:
        run_cleanup(e);
        break;
    default:
        break; /* IDLE / TERMINAL (awaiting ack) / FAULTED (owned) */
    }
}

bool mb_executor_poll_terminal(MbExecutor* e, uint64_t* job_id, MbStatus* status, MbStopReason* reason) {
    if(!e->terminal_pending) return false;
    if(job_id != NULL) *job_id = e->terminal_job_id;
    if(status != NULL) *status = e->terminal_status;
    if(reason != NULL) *reason = e->terminal_reason;
    e->terminal_pending = false;
    if(e->state == MB_JOB_TERMINAL) {
        e->state = MB_JOB_IDLE; /* slot reusable only after the ack (§9.3) */
        e->job_id = 0;
        e->module = NULL;
    }
    return true;
}

bool mb_executor_data_poll(MbExecutor* e, MbDataRecord* out) {
    if(e->q_count == 0) return false;
    uint8_t idx = e->queue[e->q_head];
    e->q_head = (e->q_head + 1) % MB_DATA_POOL_SLOTS;
    e->q_count--;
    if(out != NULL) *out = e->pool[idx].rec;
    e->pool[idx].owner = MB_OWNER_FREE;
    e->consumed++;
    return true;
}
