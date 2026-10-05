#pragma once
/* Muse Bridge IR module (plan section 10.5, checkpoint C12): the
 * first asynchronous module. Decoded-only receive: a timed job runs
 * the platform IR worker; its callback copies each decoded message
 * into a bounded SPSC ring in this (portable) module, and the
 * executor-thread service drains the ring into the job data pool.
 * Every decoded frame gets a contiguous event_seq at decode time, so
 * the host reconciles exactly: received + holes == last_seq, and the
 * retained summary carries decoded/emitted/dropped totals.
 *
 * P1-A qualifies bridge protocol id 1 (NEC) only.
 */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_IR_OP_RX_START 0x0401
#define MB_IR_PROTOCOL_NEC 1
#define MB_IR_MAX_TIMEOUT_MS 60000
#define MB_IR_RING 16

typedef struct {
    /* Platform worker lifecycle. Executed on the executor thread. */
    bool (*rx_start)(void* ctx);
    void (*rx_stop)(void* ctx); /* stop + join + free */
    void* ctx;
} MbIrHal;

typedef struct {
    uint16_t protocol_filter;
    uint32_t timeout_ms;
} MbIrParams;

typedef struct {
    uint32_t seq;
    uint8_t protocol;
    uint8_t repeat;
    uint32_t address;
    uint32_t command;
} MbIrEvent;

typedef struct {
    MbIrParams params;
    uint32_t t0;
    bool anchored;
    bool stop_seen;
    bool worker_on; /* rx_start succeeded; cleanup owes a stop */
    /* Decoded accounting (callback-maintained counters are volatile:
     * the producer is the platform worker thread). */
    uint32_t decoded; /* qualified frames seen */
    uint32_t ring_dropped; /* ring was full at decode time */
    uint32_t stranded; /* still queued when the job ended */
    uint32_t emitted; /* handed to the job data pool */
    uint32_t next_seq;
    volatile uint32_t r_head; /* producer (callback thread) */
    volatile uint32_t r_tail; /* consumer (executor thread) */
    MbIrEvent ring[MB_IR_RING];
} MbIrState;

extern const MbModule mb_module_ir;

void mb_ir_module_bind(const MbIrHal* hal);

/* Final accounting of the most recently finished RX job, snapshotted
 * at cleanup (the executor frees per-job state right after). */
typedef struct {
    uint32_t decoded;
    uint32_t emitted;
    uint32_t dropped; /* decoded - emitted: ring/pool/stranded losses */
} MbIrSummary;

void mb_ir_last_summary(MbIrSummary* out);

/* Called from the platform IR callback (worker thread): copies the
 * decoded values immediately. protocol_id is the bridge id; frames
 * not matching the active job's filter (or arriving while no RX job
 * is active) are ignored entirely. */
void mb_ir_on_decoded(uint8_t protocol_id, uint32_t address, uint32_t command, bool repeat);

bool mb_ir_validate_params(const MbIrParams* params, MbStatus* out_status);

#ifdef __cplusplus
}
#endif
