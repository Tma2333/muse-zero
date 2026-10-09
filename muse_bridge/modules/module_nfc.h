#pragma once
/* Muse Bridge NFC module (plan section 10.6 steps A/B, checkpoint
 * C16): NFC_SCAN discovers protocol candidates (not a card read);
 * NFC_IDENTIFY activates one explicitly supported protocol
 * (ISO14443-3A) and returns UID + ATQA + SAK. One NFC instance per
 * job, scanner XOR poller, all card data copied into job-owned
 * storage by the platform callback feeders below — no borrowed
 * pointer is ever retained (plan section 10.6, R09/R10).
 *
 * The platform glue resolves each raw candidate's ancestry
 * (nfc_protocol_has_parent) into a bridge protocol id, 0 =
 * unmapped; this portable core owns mapping bookkeeping (dedupe,
 * order, cap, unmapped count) plus both job state machines. */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_NFC_OP_SCAN 0x0501
#define MB_NFC_OP_IDENTIFY 0x0502
#define MB_NFC_PROTOCOL_ISO14443_3A 1
#define MB_NFC_MAX_TIMEOUT_MS 60000
#define MB_NFC_MAX_CANDIDATES 16 /* raw candidates per scanner event */
#define MB_NFC_MAX_MAPPED 4 /* mapped ids per scan summary */
#define MB_NFC_UID_MAX 10

typedef struct {
    /* Platform lifecycle, executed on the executor thread. Each
     * start allocates the NFC instance plus its scanner/poller;
     * each stop stops and frees both. */
    bool (*scan_start)(void* ctx);
    void (*scan_stop)(void* ctx);
    bool (*identify_start)(void* ctx);
    void (*identify_stop)(void* ctx);
    void* ctx;
} MbNfcHal;

void mb_nfc_module_bind(const MbNfcHal* hal);

/* Bench signals v2: the integrator's hook fires on the executor
 * thread the moment the first intended signal lands (scan: first
 * candidate list; identify: valid record). Beep = "got it" only. */
void mb_nfc_set_received_hook(void (*fn)(void* ctx), void* ctx);

/* ---- NFC_SCAN ---- */
typedef struct {
    uint32_t timeout_ms;
} MbNfcScanParams;

typedef struct {
    MbNfcScanParams params;
    uint32_t t0;
    bool anchored;
    bool stop_seen;
    bool worker_on;
    volatile bool have; /* first candidate list latched */
    uint8_t mapped_count;
    uint16_t mapped[MB_NFC_MAX_MAPPED];
    uint8_t unmapped_count;
} MbNfcScanState;

extern const MbModule mb_module_nfc_scan;

bool mb_nfc_scan_validate_params(const MbNfcScanParams* params, MbStatus* out_status);

/* Called from the platform scanner callback (any thread): the
 * glue has already translated each raw candidate to a bridge
 * protocol id (0 = unmapped). First list wins; copies immediately. */
void mb_nfc_on_candidates(const uint16_t* mapped_ids, size_t count);

typedef struct {
    bool have;
    uint8_t mapped_count;
    uint16_t mapped[MB_NFC_MAX_MAPPED];
    uint8_t unmapped_count;
    uint32_t scans_total;
} MbNfcScanSummary;

void mb_nfc_scan_last_summary(MbNfcScanSummary* out);

/* ---- NFC_IDENTIFY ---- */
typedef struct {
    uint16_t protocol; /* MB_NFC_PROTOCOL_ISO14443_3A only */
    uint32_t timeout_ms;
} MbNfcIdentifyParams;

typedef struct {
    MbNfcIdentifyParams params;
    uint32_t t0;
    bool anchored;
    bool stop_seen;
    bool worker_on;
    volatile bool have; /* a valid record was latched */
    uint8_t uid_len;
    uint8_t uid[MB_NFC_UID_MAX];
    uint8_t atqa[2];
    uint8_t sak;
    volatile uint32_t error_events;
} MbNfcIdentifyState;

extern const MbModule mb_module_nfc_identify;

bool mb_nfc_identify_validate_params(const MbNfcIdentifyParams* params, MbStatus* out_status);

/* Called from the platform poller callback on Ready (any thread):
 * copies the record immediately. UID lengths outside {4,7,10}
 * are not a valid Type-A record (plan 22.4): counted as an error
 * event, polling continues. */
void mb_nfc_on_identify(const uint8_t* uid, uint8_t uid_len, const uint8_t atqa[2], uint8_t sak);

/* Called from the platform poller callback on Error. */
void mb_nfc_on_identify_error(void);

typedef struct {
    bool found;
    uint8_t uid_len;
    uint8_t uid[MB_NFC_UID_MAX];
    uint8_t atqa[2];
    uint8_t sak;
    uint32_t error_events;
    uint32_t ids_total;
} MbNfcIdentifySummary;

void mb_nfc_identify_last_summary(MbNfcIdentifySummary* out);

#ifdef __cplusplus
}
#endif
