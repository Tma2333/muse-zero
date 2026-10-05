/* C12 native tests: IR receive ring, seq assignment, drop accounting,
 * timeout completion, and the cleanup summary snapshot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../modules/module_ir.h"

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

/* ---- Fake platform HAL ---- */
static int hal_starts, hal_stops;
static bool hal_fail_start;

static bool fake_rx_start(void* ctx) {
    (void)ctx;
    hal_starts++;
    return !hal_fail_start;
}

static void fake_rx_stop(void* ctx) {
    (void)ctx;
    hal_stops++;
}

/* ---- Emit capture ---- */
static uint8_t emitted_recs[64][14];
static int emitted_count;

static bool fake_emit(MbJobContext* job, const uint8_t* data, uint16_t len) {
    (void)job;
    if(emitted_count < 64 && len == 14) {
        memcpy(emitted_recs[emitted_count], data, 14);
    }
    emitted_count++;
    return true;
}

static MbIrState* fresh_state(MbJobContext* job) {
    MbIrState* s = calloc(1, sizeof(MbIrState));
    job->module_state = s;
    job->emit = fake_emit;
    return s;
}

int main(void) {
    MbIrHal hal = {.rx_start = fake_rx_start, .rx_stop = fake_rx_stop, .ctx = NULL};
    mb_ir_module_bind(&hal);

    /* 1: validation matrix */
    MbIrParams good = {.protocol_filter = 1, .timeout_ms = 10000};
    MbStatus st;
    CHECK(mb_module_ir.validate(&good) == MB_OK);
    MbIrParams badf = {.protocol_filter = 2, .timeout_ms = 10000};
    CHECK(!mb_ir_validate_params(&badf, &st));
    CHECK(st == MB_INVALID_ARGUMENT);
    MbIrParams badt = {.protocol_filter = 1, .timeout_ms = 0};
    CHECK(!mb_ir_validate_params(&badt, &st));
    MbIrParams badt2 = {.protocol_filter = 1, .timeout_ms = 60001};
    CHECK(!mb_ir_validate_params(&badt2, &st));
    GROUP("ir validation");

    /* 2: deliver before start is ignored */
    mb_ir_on_decoded(1, 0x10, 0x20, false);
    MbJobContext job = {0};
    MbIrState* s = fresh_state(&job);
    CHECK(mb_module_ir.start(&job, &good) == MB_OK);
    CHECK(hal_starts == 1 && s->worker_on);
    GROUP("ir start");

    /* 3: three frames drain with contiguous seqs and copied values */
    emitted_count = 0;
    mb_ir_on_decoded(1, 0x00FF00FF, 0x11, false);
    mb_ir_on_decoded(1, 0x00FF00FF, 0x12, false);
    mb_ir_on_decoded(1, 0x00FF00FF, 0x12, true);
    mb_ir_on_decoded(2, 0x99, 0x99, false); /* unqualified protocol */
    mb_module_ir.service(&job, 1000);
    CHECK(emitted_count == 3);
    uint32_t a1, c1, a2;
    memcpy(&a1, emitted_recs[0] + 6, 4);
    memcpy(&c1, emitted_recs[0] + 10, 4);
    memcpy(&a2, emitted_recs[2] + 6, 4);
    CHECK(emitted_recs[0][4] == 1 && emitted_recs[0][5] == 0);
    CHECK(emitted_recs[2][5] == 1); /* repeat flag preserved */
    CHECK(a1 == 0x00FF00FF && c1 == 0x11 && a2 == 0x00FF00FF);
    CHECK(s->decoded == 3 && s->emitted == 3);
    GROUP("ir decode drain");

    /* 4: timeout completes with OK */
    mb_module_ir.service(&job, 1000 + 9999);
    CHECK(!job.done);
    mb_module_ir.service(&job, 1000 + 10000);
    CHECK(job.done && job.done_status == MB_OK);
    CHECK(mb_module_ir.cleanup(&job) == MB_CLEAN_OK);
    CHECK(hal_stops == 1);
    MbIrSummary sum;
    mb_ir_last_summary(&sum);
    CHECK(sum.decoded == 3 && sum.emitted == 3 && sum.dropped == 0);
    free(s);
    GROUP("ir timeout + summary");

    /* 5: ring overflow drops are counted; seqs stay contiguous */
    memset(&job, 0, sizeof(job));
    s = fresh_state(&job);
    hal_starts = 0;
    CHECK(mb_module_ir.start(&job, &good) == MB_OK);
    emitted_count = 0;
    for(int i = 0; i < 20; i++) mb_ir_on_decoded(1, 0xAB, (uint32_t)i, false);
    CHECK(s->decoded == 20 && s->ring_dropped == 4);
    mb_module_ir.service(&job, 5000);
    CHECK(emitted_count == 16);
    uint32_t first_seq, last_seq, last_cmd;
    memcpy(&first_seq, emitted_recs[0], 4);
    memcpy(&last_seq, emitted_recs[15], 4);
    memcpy(&last_cmd, emitted_recs[15] + 10, 4);
    CHECK(first_seq == 1 && last_seq == 16 && last_cmd == 15);
    mb_ir_on_decoded(1, 0xAB, 0x77, false); /* two stranded at end */
    mb_ir_on_decoded(1, 0xAB, 0x78, false);
    CHECK(mb_module_ir.cleanup(&job) == MB_CLEAN_OK);
    mb_ir_last_summary(&sum);
    CHECK(sum.decoded == 22 && sum.emitted == 16 && sum.dropped == 6);
    free(s);
    GROUP("ir overflow accounting");

    /* 6: failed worker start unwinds, cleanup is a no-op stop */
    memset(&job, 0, sizeof(job));
    s = fresh_state(&job);
    int stops_before = hal_stops;
    hal_fail_start = true;
    CHECK(mb_module_ir.start(&job, &good) == MB_INIT_FAILED);
    mb_ir_on_decoded(1, 0x1, 0x2, false); /* ignored: no active job */
    CHECK(s->decoded == 0);
    CHECK(mb_module_ir.cleanup(&job) == MB_CLEAN_OK);
    CHECK(hal_stops == stops_before);
    hal_fail_start = false;
    free(s);
    GROUP("ir start failure unwind");

    printf("test_c12: all %d groups passed\n", groups);
    return 0;
}
