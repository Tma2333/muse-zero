/* Muse Bridge NFC module (plan 10.6 steps A/B, C16). Portable:
 * both job state machines and all result bookkeeping; the
 * platform lifecycle (alloc/start/stop/free of the NFC instance
 * and its scanner or poller) lives behind MbNfcHal. */
#include "module_nfc.h"

#include <string.h>

static MbNfcHal nfc_hal;
static bool nfc_hal_bound;
static MbNfcScanState* nfc_scan_active;
static MbNfcIdentifyState* nfc_id_active;
static MbNfcScanSummary nfc_scan_summary;
static MbNfcIdentifySummary nfc_id_summary;

void mb_nfc_module_bind(const MbNfcHal* hal) {
    if(hal != NULL) nfc_hal = *hal;
    nfc_hal_bound = hal != NULL;
}

/* A short beep fires on the executor thread when the first signal
 * lands (bench signal language v2: beep means "got it" only). The
 * integrator binds it; NULL = silent. */
static void (*nfc_on_received)(void* ctx);
static void* nfc_on_received_ctx;

void mb_nfc_set_received_hook(void (*fn)(void* ctx), void* ctx) {
    nfc_on_received = fn;
    nfc_on_received_ctx = ctx;
}

static void nfc_fire_received(void) {
    if(nfc_on_received != NULL) nfc_on_received(nfc_on_received_ctx);
}

/* ---------------- scan ---------------- */

bool mb_nfc_scan_validate_params(const MbNfcScanParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->timeout_ms == 0 || params->timeout_ms > MB_NFC_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

void mb_nfc_on_candidates(const uint16_t* mapped_ids, size_t count) {
    MbNfcScanState* s = nfc_scan_active;
    if(s == NULL || s->have || mapped_ids == NULL || count == 0) return;
    if(count > MB_NFC_MAX_CANDIDATES) count = MB_NFC_MAX_CANDIDATES;
    uint8_t mapped_count = 0;
    uint8_t unmapped = 0;
    uint16_t mapped[MB_NFC_MAX_MAPPED];
    for(size_t i = 0; i < count; i++) {
        uint16_t id = mapped_ids[i];
        if(id == 0) {
            if(unmapped < 0xFF) unmapped++;
            continue;
        }
        bool seen = false;
        for(uint8_t j = 0; j < mapped_count; j++) {
            if(mapped[j] == id) seen = true;
        }
        if(!seen && mapped_count < MB_NFC_MAX_MAPPED) mapped[mapped_count++] = id;
    }
    memcpy(s->mapped, mapped, sizeof(mapped));
    s->mapped_count = mapped_count;
    s->unmapped_count = unmapped;
    s->have = true; /* publish after the copy */
}

static MbStatus nfc_scan_validate_fn(const void* params) {
    MbStatus s;
    if(!nfc_hal_bound || nfc_hal.scan_start == NULL) return MB_INVALID_ARGUMENT;
    return mb_nfc_scan_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus nfc_scan_start(MbJobContext* job, const void* params) {
    MbNfcScanState* s = job->module_state;
    const MbNfcScanParams* p = params;
    if(s == NULL || p == NULL || !nfc_hal_bound) return MB_INIT_FAILED;
    s->params = *p;
    nfc_scan_active = s;
    if(!nfc_hal.scan_start(nfc_hal.ctx)) {
        nfc_scan_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void nfc_scan_service(MbJobContext* job, uint32_t now) {
    MbNfcScanState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    if(s->have) {
        nfc_fire_received();
        job->done = true;
        job->done_status = MB_OK;
        return;
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_OK; /* honest empty result */
    }
}

static void nfc_scan_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbNfcScanState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult nfc_scan_cleanup(MbJobContext* job) {
    MbNfcScanState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    if(nfc_hal_bound && s->worker_on) {
        nfc_hal.scan_stop(nfc_hal.ctx);
        s->worker_on = false;
    }
    nfc_scan_summary.have = s->have;
    nfc_scan_summary.mapped_count = s->have ? s->mapped_count : 0;
    memcpy(nfc_scan_summary.mapped, s->mapped, sizeof(s->mapped));
    if(!s->have) memset(nfc_scan_summary.mapped, 0, sizeof(nfc_scan_summary.mapped));
    nfc_scan_summary.unmapped_count = s->have ? s->unmapped_count : 0;
    nfc_scan_summary.scans_total++;
    nfc_scan_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_nfc_scan = {
    .validate = nfc_scan_validate_fn,
    .ctx_size = sizeof(MbNfcScanState),
    .start = nfc_scan_start,
    .service = nfc_scan_service,
    .request_stop = nfc_scan_request_stop,
    .cleanup = nfc_scan_cleanup,
};

void mb_nfc_scan_last_summary(MbNfcScanSummary* out) {
    if(out != NULL) *out = nfc_scan_summary;
}

/* ---------------- identify ---------------- */

bool mb_nfc_identify_validate_params(const MbNfcIdentifyParams* params, MbStatus* out_status) {
    MbStatus s = MB_OK;
    if(params == NULL || params->protocol != MB_NFC_PROTOCOL_ISO14443_3A || params->timeout_ms == 0 ||
       params->timeout_ms > MB_NFC_MAX_TIMEOUT_MS) {
        s = MB_INVALID_ARGUMENT;
    }
    if(out_status != NULL) *out_status = s;
    return s == MB_OK;
}

void mb_nfc_on_identify(const uint8_t* uid, uint8_t uid_len, const uint8_t atqa[2], uint8_t sak) {
    MbNfcIdentifyState* s = nfc_id_active;
    if(s == NULL || s->have || uid == NULL || atqa == NULL) return;
    if(uid_len != 4 && uid_len != 7 && uid_len != 10) {
        s->error_events++; /* not a valid Type-A record (plan 22.4) */
        return;
    }
    memcpy(s->uid, uid, uid_len);
    s->uid_len = uid_len;
    s->atqa[0] = atqa[0];
    s->atqa[1] = atqa[1];
    s->sak = sak;
    s->have = true; /* publish after the copy */
}

void mb_nfc_on_identify_error(void) {
    MbNfcIdentifyState* s = nfc_id_active;
    if(s != NULL) s->error_events++;
}

static MbStatus nfc_id_validate_fn(const void* params) {
    MbStatus s;
    if(!nfc_hal_bound || nfc_hal.identify_start == NULL) return MB_INVALID_ARGUMENT;
    return mb_nfc_identify_validate_params(params, &s) ? MB_OK : s;
}

static MbStatus nfc_id_start(MbJobContext* job, const void* params) {
    MbNfcIdentifyState* s = job->module_state;
    const MbNfcIdentifyParams* p = params;
    if(s == NULL || p == NULL || !nfc_hal_bound) return MB_INIT_FAILED;
    s->params = *p;
    nfc_id_active = s;
    if(!nfc_hal.identify_start(nfc_hal.ctx)) {
        nfc_id_active = NULL;
        return MB_INIT_FAILED;
    }
    s->worker_on = true;
    return MB_OK;
}

static void nfc_id_service(MbJobContext* job, uint32_t now) {
    MbNfcIdentifyState* s = job->module_state;
    if(!s->anchored) {
        s->anchored = true;
        s->t0 = now;
    }
    if(s->have) {
        nfc_fire_received();
        job->done = true;
        job->done_status = MB_OK;
        return;
    }
    if((uint32_t)(now - s->t0) >= s->params.timeout_ms) {
        job->done = true;
        job->done_status = MB_OK; /* no card: an honest empty result */
    }
}

static void nfc_id_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)reason;
    MbNfcIdentifyState* s = job->module_state;
    if(s != NULL) s->stop_seen = true;
}

static MbCleanupResult nfc_id_cleanup(MbJobContext* job) {
    MbNfcIdentifyState* s = job->module_state;
    if(s == NULL) return MB_CLEAN_OK;
    if(nfc_hal_bound && s->worker_on) {
        nfc_hal.identify_stop(nfc_hal.ctx);
        s->worker_on = false;
    }
    nfc_id_summary.found = s->have;
    nfc_id_summary.uid_len = 0;
    memset(nfc_id_summary.uid, 0, sizeof(nfc_id_summary.uid));
    nfc_id_summary.atqa[0] = 0;
    nfc_id_summary.atqa[1] = 0;
    nfc_id_summary.sak = 0;
    if(s->have) {
        nfc_id_summary.uid_len = s->uid_len;
        memcpy(nfc_id_summary.uid, s->uid, sizeof(s->uid));
        nfc_id_summary.atqa[0] = s->atqa[0];
        nfc_id_summary.atqa[1] = s->atqa[1];
        nfc_id_summary.sak = s->sak;
    }
    nfc_id_summary.error_events = s->error_events;
    nfc_id_summary.ids_total++;
    nfc_id_active = NULL;
    return MB_CLEAN_OK;
}

const MbModule mb_module_nfc_identify = {
    .validate = nfc_id_validate_fn,
    .ctx_size = sizeof(MbNfcIdentifyState),
    .start = nfc_id_start,
    .service = nfc_id_service,
    .request_stop = nfc_id_request_stop,
    .cleanup = nfc_id_cleanup,
};

void mb_nfc_identify_last_summary(MbNfcIdentifySummary* out) {
    if(out != NULL) *out = nfc_id_summary;
}
