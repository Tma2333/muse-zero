/* C16 native tests: NFC scan candidate mapping, identify record
 * discipline (copy-not-borrow, UID length gate, error counting),
 * both job state machines against a scripted fake HAL, and
 * start/stop balance over 100 cycles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../modules/module_nfc.h"

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

static int scan_starts, scan_stops, id_starts, id_stops;
static bool fail_scan_start, fail_id_start;
static int received_hooks;

static bool fake_scan_start(void* ctx) {
    (void)ctx;
    scan_starts++;
    return !fail_scan_start;
}
static void fake_scan_stop(void* ctx) {
    (void)ctx;
    scan_stops++;
}
static bool fake_id_start(void* ctx) {
    (void)ctx;
    id_starts++;
    return !fail_id_start;
}
static void fake_id_stop(void* ctx) {
    (void)ctx;
    id_stops++;
}
static void fake_received(void* ctx) {
    (void)ctx;
    received_hooks++;
}

static void bind_hal(void) {
    MbNfcHal hal = {
        .scan_start = fake_scan_start,
        .scan_stop = fake_scan_stop,
        .identify_start = fake_id_start,
        .identify_stop = fake_id_stop,
        .ctx = NULL};
    mb_nfc_module_bind(&hal);
    mb_nfc_set_received_hook(fake_received, NULL);
}

int main(void) {
    bind_hal();

    /* 1: validators */
    MbStatus st;
    MbNfcScanParams sp = {.timeout_ms = 5000};
    CHECK(mb_module_nfc_scan.validate(&sp) == MB_OK);
    sp.timeout_ms = 0;
    CHECK(!mb_nfc_scan_validate_params(&sp, &st) && st == MB_INVALID_ARGUMENT);
    sp.timeout_ms = 60001;
    CHECK(!mb_nfc_scan_validate_params(&sp, &st) && st == MB_INVALID_ARGUMENT);
    MbNfcIdentifyParams ip = {.protocol = 1, .timeout_ms = 5000};
    CHECK(mb_module_nfc_identify.validate(&ip) == MB_OK);
    ip.protocol = 2;
    CHECK(!mb_nfc_identify_validate_params(&ip, &st) && st == MB_INVALID_ARGUMENT);
    ip.protocol = 0;
    CHECK(!mb_nfc_identify_validate_params(&ip, &st) && st == MB_INVALID_ARGUMENT);
    ip.protocol = 1;
    ip.timeout_ms = 0;
    CHECK(!mb_nfc_identify_validate_params(&ip, &st) && st == MB_INVALID_ARGUMENT);
    GROUP("validators");

    /* 2: scan maps, dedupes, counts unmapped; first list wins */
    {
        MbJobContext job = {0};
        MbNfcScanState* s = calloc(1, sizeof(MbNfcScanState));
        job.module_state = s;
        MbNfcScanParams p = {.timeout_ms = 5000};
        CHECK(mb_module_nfc_scan.start(&job, &p) == MB_OK);
        uint16_t cands[5] = {1, 1, 0, 1, 0}; /* Type-A x3, unmapped x2 */
        mb_nfc_on_candidates(cands, 5);
        uint16_t late[1] = {0};
        mb_nfc_on_candidates(late, 1); /* ignored: first list wins */
        received_hooks = 0;
        mb_module_nfc_scan.service(&job, 100);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(received_hooks == 1); /* v2 received beep fired once */
        CHECK(mb_module_nfc_scan.cleanup(&job) == MB_CLEAN_OK);
        MbNfcScanSummary sum;
        mb_nfc_scan_last_summary(&sum);
        CHECK(sum.have && sum.mapped_count == 1 && sum.mapped[0] == 1);
        CHECK(sum.unmapped_count == 2 && sum.scans_total == 1);
        free(s);
    }
    GROUP("scan mapping");

    /* 3: mapped list caps at 4, order preserved */
    {
        MbJobContext job = {0};
        MbNfcScanState* s = calloc(1, sizeof(MbNfcScanState));
        job.module_state = s;
        MbNfcScanParams p = {.timeout_ms = 5000};
        CHECK(mb_module_nfc_scan.start(&job, &p) == MB_OK);
        uint16_t cands[6] = {7, 3, 9, 3, 5, 11};
        mb_nfc_on_candidates(cands, 6);
        mb_module_nfc_scan.service(&job, 100);
        CHECK(job.done);
        CHECK(mb_module_nfc_scan.cleanup(&job) == MB_CLEAN_OK);
        MbNfcScanSummary sum;
        mb_nfc_scan_last_summary(&sum);
        CHECK(sum.mapped_count == 4);
        CHECK(sum.mapped[0] == 7 && sum.mapped[1] == 3 && sum.mapped[2] == 9 && sum.mapped[3] == 5);
        CHECK(sum.unmapped_count == 0);
        free(s);
    }
    GROUP("scan mapped cap");

    /* 4: scan timeout -> OK, honest empty summary */
    {
        MbJobContext job = {0};
        MbNfcScanState* s = calloc(1, sizeof(MbNfcScanState));
        job.module_state = s;
        MbNfcScanParams p = {.timeout_ms = 1000};
        CHECK(mb_module_nfc_scan.start(&job, &p) == MB_OK);
        received_hooks = 0;
        mb_module_nfc_scan.service(&job, 500);
        CHECK(!job.done);
        mb_module_nfc_scan.service(&job, 1501);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(received_hooks == 0); /* nothing received: no beep */
        CHECK(mb_module_nfc_scan.cleanup(&job) == MB_CLEAN_OK);
        MbNfcScanSummary sum;
        mb_nfc_scan_last_summary(&sum);
        CHECK(!sum.have && sum.mapped_count == 0 && sum.unmapped_count == 0);
        free(s);
    }
    GROUP("scan timeout empty");

    /* 5: scan start failure unwinds (no stop owed) */
    {
        fail_scan_start = true;
        MbJobContext job = {0};
        MbNfcScanState* s = calloc(1, sizeof(MbNfcScanState));
        job.module_state = s;
        MbNfcScanParams p = {.timeout_ms = 1000};
        int stops_before = scan_stops;
        CHECK(mb_module_nfc_scan.start(&job, &p) == MB_INIT_FAILED);
        CHECK(mb_module_nfc_scan.cleanup(&job) == MB_CLEAN_OK);
        CHECK(scan_stops == stops_before);
        fail_scan_start = false;
        free(s);
    }
    GROUP("scan start failure");

    /* 6: identify copies the record; source mutation cannot leak */
    {
        MbJobContext job = {0};
        MbNfcIdentifyState* s = calloc(1, sizeof(MbNfcIdentifyState));
        job.module_state = s;
        MbNfcIdentifyParams p = {.protocol = 1, .timeout_ms = 5000};
        CHECK(mb_module_nfc_identify.start(&job, &p) == MB_OK);
        uint8_t uid[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        uint8_t atqa[2] = {0x04, 0x00};
        mb_nfc_on_identify(uid, 4, atqa, 0x08);
        uid[0] = 0x00; /* mutate the borrowed source after the callback */
        atqa[0] = 0xFF;
        received_hooks = 0;
        mb_module_nfc_identify.service(&job, 100);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(received_hooks == 1);
        CHECK(mb_module_nfc_identify.cleanup(&job) == MB_CLEAN_OK);
        MbNfcIdentifySummary sum;
        mb_nfc_identify_last_summary(&sum);
        CHECK(sum.found && sum.uid_len == 4);
        CHECK(sum.uid[0] == 0xDE && sum.uid[1] == 0xAD && sum.uid[2] == 0xBE && sum.uid[3] == 0xEF);
        CHECK(sum.atqa[0] == 0x04 && sum.atqa[1] == 0x00 && sum.sak == 0x08);
        CHECK(sum.error_events == 0 && sum.ids_total == 1);
        free(s);
    }
    GROUP("identify copy discipline");

    /* 7: invalid UID length is an error event, polling continues */
    {
        MbJobContext job = {0};
        MbNfcIdentifyState* s = calloc(1, sizeof(MbNfcIdentifyState));
        job.module_state = s;
        MbNfcIdentifyParams p = {.protocol = 1, .timeout_ms = 5000};
        CHECK(mb_module_nfc_identify.start(&job, &p) == MB_OK);
        uint8_t bad[5] = {1, 2, 3, 4, 5};
        uint8_t atqa[2] = {0x44, 0x00};
        mb_nfc_on_identify(bad, 5, atqa, 0x00);
        mb_nfc_on_identify_error();
        mb_module_nfc_identify.service(&job, 100);
        CHECK(!job.done); /* still polling */
        uint8_t uid7[7] = {1, 2, 3, 4, 5, 6, 7};
        mb_nfc_on_identify(uid7, 7, atqa, 0x00);
        mb_module_nfc_identify.service(&job, 200);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(mb_module_nfc_identify.cleanup(&job) == MB_CLEAN_OK);
        MbNfcIdentifySummary sum;
        mb_nfc_identify_last_summary(&sum);
        CHECK(sum.found && sum.uid_len == 7 && sum.error_events == 2);
        free(s);
    }
    GROUP("identify uid gate + errors");

    /* 8: identify timeout -> found=0, zeroed record, errors kept */
    {
        MbJobContext job = {0};
        MbNfcIdentifyState* s = calloc(1, sizeof(MbNfcIdentifyState));
        job.module_state = s;
        MbNfcIdentifyParams p = {.protocol = 1, .timeout_ms = 1000};
        CHECK(mb_module_nfc_identify.start(&job, &p) == MB_OK);
        mb_nfc_on_identify_error();
        mb_nfc_on_identify_error();
        mb_nfc_on_identify_error();
        mb_module_nfc_identify.service(&job, 500); /* anchor */
        CHECK(!job.done);
        mb_module_nfc_identify.service(&job, 1501);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(mb_module_nfc_identify.cleanup(&job) == MB_CLEAN_OK);
        MbNfcIdentifySummary sum;
        mb_nfc_identify_last_summary(&sum);
        CHECK(!sum.found && sum.uid_len == 0 && sum.error_events == 3);
        for(int i = 0; i < MB_NFC_UID_MAX; i++) CHECK(sum.uid[i] == 0);
        free(s);
    }
    GROUP("identify timeout empty");

    /* 9: cancel mid-window frees the platform exactly once */
    {
        MbJobContext job = {0};
        MbNfcIdentifyState* s = calloc(1, sizeof(MbNfcIdentifyState));
        job.module_state = s;
        MbNfcIdentifyParams p = {.protocol = 1, .timeout_ms = 60000};
        int stops_before = id_stops;
        CHECK(mb_module_nfc_identify.start(&job, &p) == MB_OK);
        mb_module_nfc_identify.request_stop(&job, MB_STOP_CANCEL);
        CHECK(mb_module_nfc_identify.cleanup(&job) == MB_CLEAN_OK);
        CHECK(id_stops == stops_before + 1);
        free(s);
    }
    GROUP("identify cancel cleanup");

    /* 10: 100 alternating cycles, balanced alloc/free, stable data */
    {
        uint8_t uid[4] = {0x11, 0x22, 0x33, 0x44};
        uint8_t atqa[2] = {0x04, 0x00};
        int sb = scan_starts, ib = id_starts;
        int sstop_b = scan_stops, istop_b = id_stops;
        for(int i = 0; i < 100; i++) {
            if(i % 2 == 0) {
                MbJobContext job = {0};
                MbNfcScanState* s = calloc(1, sizeof(MbNfcScanState));
                job.module_state = s;
                MbNfcScanParams p = {.timeout_ms = 100};
                CHECK(mb_module_nfc_scan.start(&job, &p) == MB_OK);
                uint16_t c[1] = {1};
                mb_nfc_on_candidates(c, 1);
                mb_module_nfc_scan.service(&job, 10);
                CHECK(job.done);
                CHECK(mb_module_nfc_scan.cleanup(&job) == MB_CLEAN_OK);
                free(s);
            } else {
                MbJobContext job = {0};
                MbNfcIdentifyState* s = calloc(1, sizeof(MbNfcIdentifyState));
                job.module_state = s;
                MbNfcIdentifyParams p = {.protocol = 1, .timeout_ms = 100};
                CHECK(mb_module_nfc_identify.start(&job, &p) == MB_OK);
                mb_nfc_on_identify(uid, 4, atqa, 0x08);
                mb_module_nfc_identify.service(&job, 10);
                CHECK(job.done);
                CHECK(mb_module_nfc_identify.cleanup(&job) == MB_CLEAN_OK);
                MbNfcIdentifySummary sum;
                mb_nfc_identify_last_summary(&sum);
                CHECK(sum.found && sum.uid[0] == 0x11 && sum.uid[3] == 0x44);
                free(s);
            }
        }
        CHECK(scan_starts - sb == 50 && id_starts - ib == 50);
        CHECK(scan_stops - sstop_b == 50 && id_stops - istop_b == 50);
    }
    GROUP("100 cycles balanced");

    printf("PASS test_c16 (%d groups)\n", groups);
    return 0;
}
