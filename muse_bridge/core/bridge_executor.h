#pragma once
/* Muse Bridge hardware executor (plan §5.1, §9).
 * Sole owner of the module lifecycle for the one exclusive job slot
 * (§9.2). Driven by its owning thread calling mb_executor_service();
 * the controller admits jobs, requests stops through an independent
 * latch (never through the data queue), drains data, and acks exactly
 * one terminal outcome per job. No Furi dependency.
 */
#include "bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_DATA_POOL_SLOTS 8

typedef enum { MB_OWNER_FREE = 0, MB_OWNER_PRODUCER, MB_OWNER_CONSUMER } MbPoolOwner;

typedef struct {
    MbDataRecord rec;
    MbPoolOwner owner;
} MbPoolSlot;

typedef struct {
    const MbAlloc* alloc;
    /* the exclusive job slot */
    MbJobState state;
    const MbModule* module;
    const void* job_params; /* retained from admit; caller keeps it alive */
    MbJobContext ctx;
    uint64_t job_id;
    uint32_t admit_tick;
    uint32_t deadline_ticks; /* 0 = none; absolute from admit, never extended */
    volatile bool stop_latch;
    MbStopReason stop_reason;
    bool stop_notified; /* module request_stop called exactly once */
    bool started; /* module start succeeded (cleanup is owed) */
    MbStatus pending_status; /* terminal outcome under construction */
    MbStopReason pending_reason;
    /* non-droppable completion latch (§5.2 rule 7) */
    bool terminal_pending;
    uint64_t terminal_job_id;
    MbStatus terminal_status;
    MbStopReason terminal_reason;
    /* bounded data pool + FIFO (§8): seq counted at generation */
    MbPoolSlot pool[MB_DATA_POOL_SLOTS];
    uint8_t queue[MB_DATA_POOL_SLOTS];
    unsigned q_head, q_count;
    uint32_t next_seq;
    uint32_t generated, enqueued, dropped, consumed, last_generated_seq;
    /* diagnostics */
    uint32_t starts, terminals;
} MbExecutor;

void mb_executor_init(MbExecutor* e, const MbAlloc* alloc);
void mb_executor_deinit(MbExecutor* e); /* app teardown only; frees retained state */

/* -> MB_OK and MB_JOB_ACCEPTED, or MB_BUSY when the slot is occupied
 * (including TERMINAL-unacked and FAULTED), or the module's validation
 * result. A refused admit consumes nothing. */
MbStatus mb_executor_admit(
    MbExecutor* e,
    uint64_t job_id,
    const MbModule* module,
    const void* params,
    uint32_t now,
    uint32_t deadline_ticks);

/* Executor-thread pump. Bounded work per call. */
void mb_executor_service(MbExecutor* e, uint32_t now);

/* Independent stop latch: callable from any role, never blocks on the
 * data path. Returns false when no job is active. */
bool mb_executor_request_stop(MbExecutor* e, MbStopReason reason);

/* Job-targeted stop (C08): latches only when job_id is the executor's
 * current job. A stale cancel aimed at a finished job returns false and
 * can never stop a newer job that reused the slot. */
bool mb_executor_request_stop_job(MbExecutor* e, uint64_t job_id, MbStopReason reason);

bool mb_executor_poll_terminal(MbExecutor* e, uint64_t* job_id, MbStatus* status, MbStopReason* reason);
bool mb_executor_data_poll(MbExecutor* e, MbDataRecord* out);
bool mb_executor_busy(const MbExecutor* e);
MbJobState mb_executor_state(const MbExecutor* e);

#ifdef __cplusplus
}
#endif
