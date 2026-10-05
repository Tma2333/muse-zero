/*
 * bridge_diag.c - C02 bounded diagnostic trace ring.
 * See bridge_diag.h for the contract (plan section 18.1 / C02).
 */
#include "bridge_diag.h"

#include <string.h>

_Static_assert(sizeof(MbTraceRecord) == 32, "trace record must be 32 bytes");
_Static_assert(
    sizeof(MbDiag) <= 64u * 32u + 64u,
    "trace storage must stay ~2 KiB plus indices");

void mb_diag_init(MbDiag* diag) {
    memset(diag, 0, sizeof(*diag));
    diag->next_seq = 1;
}

void mb_diag_new_session(MbDiag* diag) {
    diag->session_epoch++;
}

void mb_diag_push(
    MbDiag* diag,
    uint32_t tick,
    uint16_t event_code,
    uint64_t job_id,
    uint32_t connection_generation,
    uint32_t arg0,
    uint32_t arg1) {
    MbTraceRecord* rec = &diag->records[diag->write_idx];
    rec->trace_seq = diag->next_seq;
    rec->tick = tick;
    rec->job_id = job_id;
    rec->connection_generation = connection_generation;
    rec->arg0 = arg0;
    rec->arg1 = arg1;
    rec->event_code = event_code;
    rec->session_epoch = diag->session_epoch;

    diag->next_seq++;
    diag->write_idx = (diag->write_idx + 1u) % MB_TRACE_CAPACITY;
    if(diag->count < MB_TRACE_CAPACITY) {
        diag->count++;
    } else {
        diag->overwrites++;
    }
}

static uint32_t mb_diag_oldest_seq(const MbDiag* diag) {
    if(diag->count == 0) return 0;
    return diag->next_seq - diag->count;
}

void mb_diag_snapshot(const MbDiag* diag, MbDiagSnapshot* out) {
    out->count = diag->count;
    out->overwrites = diag->overwrites;
    out->wrapped = diag->overwrites > 0;
    out->session_epoch = diag->session_epoch;
    out->oldest_seq = mb_diag_oldest_seq(diag);
    out->newest_seq = diag->count ? diag->next_seq - 1u : 0;
}

uint32_t mb_diag_read_after(
    const MbDiag* diag,
    uint32_t after_seq,
    MbTraceRecord* out,
    uint32_t max,
    bool* gap,
    bool* has_more) {
    uint32_t oldest = mb_diag_oldest_seq(diag);
    uint32_t newest = diag->count ? diag->next_seq - 1u : 0;
    *gap = false;
    *has_more = false;
    if(diag->count == 0 || max == 0) {
        *gap = after_seq > newest; /* asked beyond anything recorded */
        return 0;
    }
    if(after_seq >= newest) {
        *gap = after_seq > newest;
        return 0;
    }
    uint32_t start = after_seq + 1u;
    if(start < oldest) {
        *gap = true; /* the requested window was partly overwritten */
        start = oldest;
    }
    uint32_t available = newest - start + 1u;
    uint32_t n = available < max ? available : max;
    /* Records are stored by seq modulo capacity: seq s sits in slot
     * (s - 1) % MB_TRACE_CAPACITY because seq 1 landed in slot 0 and the
     * write index advances with seq. */
    for(uint32_t i = 0; i < n; i++) {
        out[i] = diag->records[(start + i - 1u) % MB_TRACE_CAPACITY];
    }
    *has_more = (start + n - 1u) < newest;
    return n;
}
