/* C13 native tests: finite NEC TX provider discipline (one NEW then
 * STOP forever), completion on provider exhaustion, timeout
 * truthfulness, stop/start-failure unwind, summary snapshot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../modules/module_ir.h"

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

/* ---- Fake platform HAL ---- */
static int hal_tx_starts, hal_tx_stops;
static bool hal_fail_start;

static bool fake_tx_start(void* ctx) {
    (void)ctx;
    hal_tx_starts++;
    return !hal_fail_start;
}

static void fake_tx_stop(void* ctx) {
    (void)ctx;
    hal_tx_stops++;
}

static bool fake_rx_start(void* ctx) {
    (void)ctx;
    return true;
}

static void fake_rx_stop(void* ctx) {
    (void)ctx;
}

static MbIrTxState* fresh_state(MbJobContext* job) {
    MbIrTxState* s = calloc(1, sizeof(MbIrTxState));
    job->module_state = s;
    return s;
}

static const MbIrTxParams GOOD = {
    .protocol = 1, .address = 0x04, .command = 0x08, .frame_count = 1, .timeout_ms = 5000};

int main(void) {
    MbIrHal hal = {
        .rx_start = fake_rx_start,
        .rx_stop = fake_rx_stop,
        .tx_start = fake_tx_start,
        .tx_stop = fake_tx_stop,
        .ctx = NULL};
    mb_ir_module_bind(&hal);

    /* 1: validation matrix */
    MbStatus st;
    CHECK(mb_module_ir_tx.validate(&GOOD) == MB_OK);
    MbIrTxParams bad = GOOD;
    bad.protocol = 2;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.frame_count = 0;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.frame_count = 2;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.address = 0x100;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.command = 0x1FF;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.timeout_ms = 0;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    bad = GOOD;
    bad.timeout_ms = 60001;
    CHECK(!mb_ir_tx_validate_params(&bad, &st) && st == MB_INVALID_ARGUMENT);
    GROUP("tx validation");

    /* 2: provider discipline — one NEW, then STOP forever */
    MbJobContext job = {0};
    MbIrTxState* s = fresh_state(&job);
    uint8_t proto = 0;
    uint32_t addr = 0, cmd = 0;
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_STOP); /* no job yet */
    CHECK(mb_module_ir_tx.start(&job, &GOOD) == MB_OK);
    CHECK(hal_tx_starts == 1 && s->worker_on);
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_NEW);
    CHECK(proto == 1 && addr == 0x04 && cmd == 0x08);
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_STOP);
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_STOP);
    CHECK(s->supplied == 1 && s->exhausted);
    GROUP("tx provider finite");

    /* 3: exhaustion completes the job (the platform sent callback
     * never fires for a lone frame); the cleanup join confirms the
     * supplied frame as sent in the summary. */
    mb_module_ir_tx.service(&job, 1000);
    CHECK(job.done && job.done_status == MB_OK);
    CHECK(mb_module_ir_tx.cleanup(&job) == MB_CLEAN_OK);
    CHECK(hal_tx_stops == 1);
    MbIrTxSummary sum;
    mb_ir_tx_last_summary(&sum);
    CHECK(sum.supplied == 1 && sum.sent == 1);
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_STOP); /* no active job */
    free(s);
    GROUP("tx completion + summary");

    /* 4: provider never exhausts -> timeout reported truthfully,
     * and the summary does not upgrade the frame to sent */
    memset(&job, 0, sizeof(job));
    s = fresh_state(&job);
    CHECK(mb_module_ir_tx.start(&job, &GOOD) == MB_OK);
    CHECK(mb_ir_tx_supply(&proto, &addr, &cmd) == MB_IR_TX_NEW);
    /* no second supply call: the worker never asks again */
    mb_module_ir_tx.service(&job, 2000); /* anchor */
    mb_module_ir_tx.service(&job, 2000 + 4999);
    CHECK(!job.done);
    mb_module_ir_tx.service(&job, 2000 + 5000);
    CHECK(job.done && job.done_status == MB_TIMEOUT);
    CHECK(mb_module_ir_tx.cleanup(&job) == MB_CLEAN_OK);
    mb_ir_tx_last_summary(&sum);
    CHECK(sum.supplied == 1 && sum.sent == 0);
    free(s);
    GROUP("tx timeout");

    /* 5: local stop before any supply: cleanup stops worker once */
    memset(&job, 0, sizeof(job));
    s = fresh_state(&job);
    int stops_before = hal_tx_stops;
    CHECK(mb_module_ir_tx.start(&job, &GOOD) == MB_OK);
    mb_module_ir_tx.request_stop(&job, MB_STOP_LOCAL);
    CHECK(s->stop_seen);
    CHECK(mb_module_ir_tx.cleanup(&job) == MB_CLEAN_OK);
    CHECK(hal_tx_stops == stops_before + 1);
    mb_ir_tx_last_summary(&sum);
    CHECK(sum.supplied == 0 && sum.sent == 0);
    free(s);
    GROUP("tx stop unwind");

    /* 6: failed start owes no stop; stray callbacks are inert */
    memset(&job, 0, sizeof(job));
    s = fresh_state(&job);
    stops_before = hal_tx_stops;
    hal_fail_start = true;
    CHECK(mb_module_ir_tx.start(&job, &GOOD) == MB_INIT_FAILED);
    hal_fail_start = false;
    mb_ir_tx_on_sent(); /* no active job: ignored */
    CHECK(mb_module_ir_tx.cleanup(&job) == MB_CLEAN_OK);
    CHECK(hal_tx_stops == stops_before);
    free(s);
    GROUP("tx start failure");

    printf("test_c13: all %d groups passed\n", groups);
    return 0;
}
