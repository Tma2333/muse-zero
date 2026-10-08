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
#define MB_IR_OP_TX_DECODED 0x0402
#define MB_IR_PROTOCOL_NEC 1
#define MB_IR_MAX_TIMEOUT_MS 60000
#define MB_IR_RING 16

typedef struct {
    /* Platform worker lifecycle. Executed on the executor thread. */
    bool (*rx_start)(void* ctx);
    void (*rx_stop)(void* ctx); /* stop + join + free */
    bool (*tx_start)(void* ctx);
    void (*tx_stop)(void* ctx); /* stop (waits out current signal) + free */
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

/* ---- C13: one finite decoded transmission (plan 10.5 / R08) ------
 * The finite provider hands out the single configured frame exactly
 * once (MB_IR_TX_NEW) and then reports exhaustion (MB_IR_TX_STOP) on
 * every later call; it never returns a steady "same" response, so the
 * platform worker cannot loop it. Provider exhaustion, the platform's
 * message-sent callback, and executor stop/join are distinct states.
 *
 * Completion semantics (measured against the 1.4.3 worker source):
 * the worker thread computes NEC's minimum repeat count (1) and
 * decrements it for the first message, which therefore never raises
 * the message-sent event — for a lone frame that callback is
 * unreachable by design. So the job completes when the provider is
 * exhausted (the frame is fully encoded into the worker's stream),
 * and physical completion is established at cleanup: the platform TX
 * stop waits out the in-flight signal and joins the worker before
 * the executor records the terminal, so a job that finished via
 * exhaustion reports every supplied frame as sent, while a job that
 * timed out or was stopped keeps the raw callback count. Physical
 * proof still belongs to the independent receiver, never to these
 * counters. Initial qualification: NEC, frame_count=1, standard
 * (8-bit) address/command widths. */
typedef struct {
    uint16_t protocol;
    uint32_t address;
    uint32_t command;
    uint8_t frame_count;
    uint32_t timeout_ms;
} MbIrTxParams;

typedef enum {
    MB_IR_TX_STOP = 0,
    MB_IR_TX_NEW = 1,
} MbIrTxSupply;

typedef struct {
    MbIrTxParams params;
    uint32_t t0;
    bool anchored;
    bool stop_seen;
    bool worker_on; /* tx_start succeeded; cleanup owes a stop */
    volatile uint32_t supplied; /* frames handed to the worker (0/1) */
    volatile bool exhausted; /* provider has returned STOP */
    volatile uint32_t sent; /* platform message-sent callbacks */
    bool finished; /* service completed the job via exhaustion */
} MbIrTxState;

extern const MbModule mb_module_ir_tx;

bool mb_ir_tx_validate_params(const MbIrTxParams* params, MbStatus* out_status);

/* Called from the platform get-signal callback (worker thread): the
 * first call for the active TX job copies the frame out and returns
 * MB_IR_TX_NEW; every later call (including prefetch and calls with
 * no active job) returns MB_IR_TX_STOP. */
MbIrTxSupply mb_ir_tx_supply(uint8_t* protocol, uint32_t* address, uint32_t* command);

/* Called from the platform message-sent callback (worker thread). */
void mb_ir_tx_on_sent(void);

typedef struct {
    uint32_t supplied;
    uint32_t sent;
} MbIrTxSummary;

void mb_ir_tx_last_summary(MbIrTxSummary* out);

#ifdef __cplusplus
}
#endif
