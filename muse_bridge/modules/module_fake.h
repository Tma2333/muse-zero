#pragma once
/* Qualification-only fake hardware module (plan §18 C05).
 * Performs timed jobs and generates bounded data records; stands in
 * for real modules until each qualifies. Compiled only under
 * BRIDGE_QUALIFICATION_BUILD and never reachable from the wire until
 * C06/C07 pass. */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t duration_ticks; /* natural run length from first service */
    uint32_t emit_interval_ticks; /* 0 = no data */
    uint16_t emit_size; /* bytes per record, <= MB_DATA_PAYLOAD */
    bool fail_worker; /* start fails after staging alloc (unwind test) */
    bool fail_cleanup; /* cleanup reports failure (FAULTED test) */
} MbFakeParams;

extern const MbModule mb_module_fake;

/* Qualification-only counter (plan §4 bridge_diag): how many fake jobs
 * actually started executing. Duplicate/stale admissions must not
 * move it. */
uint32_t mb_fake_execution_count(void);
void mb_fake_execution_count_reset(void);

#ifdef __cplusplus
}
#endif
