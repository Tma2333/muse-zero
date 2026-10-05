/*
 * bridge_diag.h - C02 bounded diagnostic trace.
 *
 * Fixed 64-entry x 32-byte trace ring (plan section 18.1 / C02). The ring
 * overwrites its oldest record and counts overwrites; it never owns action
 * outcomes. Writers append from thread context inside a short critical
 * section; ISR callbacks only bump counters and never touch the ring.
 *
 * Record layout (plan order, exactly 32 bytes):
 *   trace_seq:u32, tick:u32, job_id:u64, connection_generation:u32,
 *   arg0:u32, arg1:u32, event_code:u16, session_epoch:u16
 *
 * Portable C11; the caller supplies ticks (from any monotonic source) and
 * converts with the tick frequency published in GET_INFO.
 */
#ifndef BRIDGE_DIAG_H
#define BRIDGE_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_TRACE_CAPACITY 64u

typedef enum {
    MB_EV_APP_START = 1,
    MB_EV_UART_ACQUIRED = 2,
    MB_EV_FRAME_DROP = 3,
    MB_EV_SESSION_LIVE = 4,
    MB_EV_LINK_SUSPECT = 5,
    MB_EV_LEASE_EXPIRED = 6,
    MB_EV_ACTION_ADMITTED = 7,
    MB_EV_ACTION_REPLAYED = 8,
    MB_EV_ACTION_REJECTED = 9,
    MB_EV_JOB_STARTED = 10,
    MB_EV_STOP_REQUESTED = 11,
    MB_EV_HW_QUIESCENT = 12,
    MB_EV_RESOURCE_RELEASED = 13,
    MB_EV_JOB_TERMINAL = 14,
    MB_EV_FAULT = 15,
    MB_EV_APP_EXIT = 16,
} MbEventCode;

typedef struct {
    uint32_t trace_seq;
    uint32_t tick;
    uint64_t job_id;
    uint32_t connection_generation;
    uint32_t arg0;
    uint32_t arg1;
    uint16_t event_code;
    uint16_t session_epoch;
} MbTraceRecord; /* exactly 32 bytes; enforced below */

typedef struct {
    uint32_t oldest_seq; /* 0 when empty */
    uint32_t newest_seq; /* 0 when empty */
    uint32_t count;      /* records currently held (<= MB_TRACE_CAPACITY) */
    uint32_t overwrites; /* records lost to wrap since init */
    bool wrapped;        /* overwrites > 0 */
    uint16_t session_epoch;
} MbDiagSnapshot;

typedef struct {
    MbTraceRecord records[MB_TRACE_CAPACITY];
    uint32_t next_seq; /* next trace_seq to assign (starts at 1) */
    uint32_t write_idx; /* slot the next record lands in */
    uint32_t count;
    uint32_t overwrites;
    uint16_t session_epoch;
} MbDiag;

void mb_diag_init(MbDiag* diag);
/* Starts a new session within this boot: bumps the diagnostic epoch that
 * is stamped onto subsequent records. Not a wire authorization token. */
void mb_diag_new_session(MbDiag* diag);

void mb_diag_push(
    MbDiag* diag,
    uint32_t tick,
    uint16_t event_code,
    uint64_t job_id,
    uint32_t connection_generation,
    uint32_t arg0,
    uint32_t arg1);

void mb_diag_snapshot(const MbDiag* diag, MbDiagSnapshot* out);

/* Copy up to `max` records with trace_seq > after_seq, oldest first.
 * Returns the number copied. *gap is set when the caller asked for records
 * that have already been overwritten (after_seq + 1 < oldest available),
 * or when after_seq is ahead of the newest record. *has_more is set when
 * more records remain beyond the copied window. */
uint32_t mb_diag_read_after(
    const MbDiag* diag,
    uint32_t after_seq,
    MbTraceRecord* out,
    uint32_t max,
    bool* gap,
    bool* has_more);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_DIAG_H */
