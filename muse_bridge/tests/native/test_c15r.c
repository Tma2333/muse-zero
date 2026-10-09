/* C15 native tests: raw RX capture staging + finite raw TX provider
 * (supply-once, exhaustion completion, timeout truthfulness, pin
 * discipline, over-cap terminal). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../modules/module_ir.h"

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

static int hal_rx_starts, hal_rx_stops, hal_tx_starts, hal_tx_stops;
static int hook_calls;
static void fake_received_hook(void* ctx) {
    (void)ctx;
    hook_calls++;
}
static bool hal_fail_raw_start;
static int pin_calls, unpin_calls;
static bool pin_fail;
static uint64_t last_pinned;

static bool fake_rx_start(void* ctx) { (void)ctx; return true; }
static bool fake_rx_start_raw(void* ctx) {
    (void)ctx;
    hal_rx_starts++;
    return !hal_fail_raw_start;
}
static void fake_rx_stop(void* ctx) { (void)ctx; hal_rx_stops++; }
static bool fake_tx_start(void* ctx) { (void)ctx; hal_tx_starts++; return true; }
static void fake_tx_stop(void* ctx) { (void)ctx; hal_tx_stops++; }
static bool fake_pin(void* ctx, uint64_t id) {
    (void)ctx;
    pin_calls++;
    last_pinned = id;
    return !pin_fail;
}
static void fake_unpin(void* ctx, uint64_t id) {
    (void)ctx;
    (void)id;
    unpin_calls++;
}

int main(void) {
    MbIrHal hal = {
        .rx_start = fake_rx_start,
        .rx_start_raw = fake_rx_start_raw,
        .rx_stop = fake_rx_stop,
        .tx_start = fake_tx_start,
        .tx_stop = fake_tx_stop,
        .ctx = NULL};
    mb_ir_module_bind(&hal);
    mb_ir_set_received_hook(fake_received_hook, NULL);
    MbIrTxRawBind bind = {.pin = fake_pin, .unpin = fake_unpin, .ctx = NULL};
    mb_ir_tx_raw_bind(&bind);

    /* 1: raw RX validation */
    MbStatus st;
    MbIrRawParams rp = {.timeout_ms = 5000};
    CHECK(mb_module_ir_raw_rx.validate(&rp) == MB_OK);
    rp.timeout_ms = 0;
    CHECK(!mb_ir_raw_validate_params(&rp, &st) && st == MB_INVALID_ARGUMENT);
    rp.timeout_ms = 60001;
    CHECK(!mb_ir_raw_validate_params(&rp, &st) && st == MB_INVALID_ARGUMENT);
    rp.timeout_ms = 60000;
    CHECK(mb_ir_raw_validate_params(&rp, &st) && st == MB_OK);
    GROUP("raw rx validation");

    /* 2: capture flow, first burst wins */
    {
        MbJobContext job = {0};
        MbIrRawRxState* s = calloc(1, sizeof(MbIrRawRxState));
        job.module_state = s;
        MbIrRawParams p = {.timeout_ms = 5000};
        CHECK(mb_module_ir_raw_rx.start(&job, &p) == MB_OK);
        uint32_t wave[68];
        for(int i = 0; i < 68; i++) wave[i] = 500 + (uint32_t)i;
        mb_ir_raw_on_timings(wave, 68);
        uint32_t junk[10] = {0};
        mb_ir_raw_on_timings(junk, 10); /* second burst ignored */
        mb_module_ir_raw_rx.service(&job, 100);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(mb_module_ir_raw_rx.cleanup(&job) == MB_CLEAN_OK);
        MbIrRawSummary sum;
        mb_ir_raw_last_summary(&sum);
        CHECK(sum.have && sum.count == 68 && sum.stored == 68);
        CHECK(sum.timings != NULL && sum.timings[67] == 500 + 67);
        CHECK(sum.captures_total == 1 && sum.over_cap_total == 0);
        free(s);
    }
    GROUP("raw rx capture");

    /* 3: over-cap capture -> OVERFLOW terminal, staged count kept */
    {
        MbJobContext job = {0};
        MbIrRawRxState* s = calloc(1, sizeof(MbIrRawRxState));
        job.module_state = s;
        MbIrRawParams p = {.timeout_ms = 5000};
        CHECK(mb_module_ir_raw_rx.start(&job, &p) == MB_OK);
        static uint32_t big[600];
        for(int i = 0; i < 600; i++) big[i] = 100;
        mb_ir_raw_on_timings(big, 600);
        mb_module_ir_raw_rx.service(&job, 100);
        CHECK(job.done && job.done_status == MB_OVERFLOW);
        CHECK(mb_module_ir_raw_rx.cleanup(&job) == MB_CLEAN_OK);
        MbIrRawSummary sum;
        mb_ir_raw_last_summary(&sum);
        CHECK(sum.have && sum.count == 600 && sum.stored == 600);
        CHECK(sum.over_cap_total == 1);
        free(s);
    }
    GROUP("raw rx over-cap");

    /* 4: timeout with no signal -> OK, no capture */
    {
        MbJobContext job = {0};
        MbIrRawRxState* s = calloc(1, sizeof(MbIrRawRxState));
        job.module_state = s;
        MbIrRawParams p = {.timeout_ms = 1000};
        CHECK(mb_module_ir_raw_rx.start(&job, &p) == MB_OK);
        mb_module_ir_raw_rx.service(&job, 5000);
        CHECK(!job.done);
        mb_module_ir_raw_rx.service(&job, 6001);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(mb_module_ir_raw_rx.cleanup(&job) == MB_CLEAN_OK);
        MbIrRawSummary sum;
        mb_ir_raw_last_summary(&sum);
        CHECK(!sum.have && sum.count == 0);
        free(s);
    }
    GROUP("raw rx timeout");

    /* 5: raw RX start failure unwinds */
    {
        hal_fail_raw_start = true;
        MbJobContext job = {0};
        MbIrRawRxState* s = calloc(1, sizeof(MbIrRawRxState));
        job.module_state = s;
        MbIrRawParams p = {.timeout_ms = 1000};
        CHECK(mb_module_ir_raw_rx.start(&job, &p) == MB_INIT_FAILED);
        int stops_before = hal_rx_stops;
        CHECK(mb_module_ir_raw_rx.cleanup(&job) == MB_CLEAN_OK);
        CHECK(hal_rx_stops == stops_before); /* never started: no stop owed */
        hal_fail_raw_start = false;
        free(s);
    }
    GROUP("raw rx start failure");

    /* 6: raw TX validation */
    MbIrTxRawParams tp = {.object_id = 42, .frame_count = 1, .timeout_ms = 5000};
    CHECK(mb_module_ir_tx_raw.validate(&tp) == MB_OK);
    tp.frame_count = 2;
    CHECK(!mb_ir_tx_raw_validate_params(&tp, &st) && st == MB_INVALID_ARGUMENT);
    tp.frame_count = 1;
    tp.timeout_ms = 0;
    CHECK(!mb_ir_tx_raw_validate_params(&tp, &st) && st == MB_INVALID_ARGUMENT);
    tp.timeout_ms = 5000;
    GROUP("raw tx validation");

    /* 7: raw TX full flow */
    {
        uint32_t wave[68];
        for(int i = 0; i < 68; i++) wave[i] = 9000 - (uint32_t)i;
        mb_ir_tx_raw_stage(wave, 68);
        MbJobContext job = {0};
        MbIrTxRawState* s = calloc(1, sizeof(MbIrTxRawState));
        job.module_state = s;
        MbIrTxRawParams p = {.object_id = 42, .frame_count = 1, .timeout_ms = 5000};
        CHECK(mb_module_ir_tx_raw.start(&job, &p) == MB_OK);
        CHECK(pin_calls == 1 && last_pinned == 42);
        const uint32_t* out_t = NULL;
        uint16_t out_c = 0;
        CHECK(mb_ir_tx_raw_supply(&out_t, &out_c) == MB_IR_TX_NEW);
        CHECK(out_c == 68 && out_t != NULL && out_t[0] == 9000 && out_t[67] == 9000 - 67);
        CHECK(mb_ir_tx_raw_supply(&out_t, &out_c) == MB_IR_TX_STOP);
        mb_module_ir_tx_raw.service(&job, 100);
        CHECK(job.done && job.done_status == MB_OK);
        CHECK(mb_module_ir_tx_raw.cleanup(&job) == MB_CLEAN_OK);
        MbIrTxRawSummary sum;
        mb_ir_tx_raw_last_summary(&sum);
        CHECK(sum.supplied == 1 && sum.sent == 1 && sum.tx_total == 1);
        CHECK(unpin_calls == 1);
        free(s);
    }
    GROUP("raw tx flow");

    /* 8: raw TX timeout keeps the raw (zero) sent count */
    {
        uint32_t wave[4] = {100, 100, 100, 100};
        mb_ir_tx_raw_stage(wave, 4);
        MbJobContext job = {0};
        MbIrTxRawState* s = calloc(1, sizeof(MbIrTxRawState));
        job.module_state = s;
        MbIrTxRawParams p = {.object_id = 7, .frame_count = 1, .timeout_ms = 1000};
        CHECK(mb_module_ir_tx_raw.start(&job, &p) == MB_OK);
        mb_module_ir_tx_raw.service(&job, 500);
        CHECK(!job.done);
        mb_module_ir_tx_raw.service(&job, 2000);
        CHECK(job.done && job.done_status == MB_TIMEOUT);
        CHECK(mb_module_ir_tx_raw.cleanup(&job) == MB_CLEAN_OK);
        MbIrTxRawSummary sum;
        mb_ir_tx_raw_last_summary(&sum);
        CHECK(sum.supplied == 0 && sum.sent == 0 && sum.tx_total == 1);
        free(s);
    }
    GROUP("raw tx timeout");

    /* 9: stage is single-use; pin failure prevents TX start */
    {
        MbJobContext job = {0};
        MbIrTxRawState* s = calloc(1, sizeof(MbIrTxRawState));
        job.module_state = s;
        MbIrTxRawParams p = {.object_id = 7, .frame_count = 1, .timeout_ms = 1000};
        CHECK(mb_module_ir_tx_raw.start(&job, &p) == MB_INIT_FAILED); /* nothing staged */
        free(s);
        uint32_t wave[4] = {1, 2, 3, 4};
        mb_ir_tx_raw_stage(wave, 4);
        pin_fail = true;
        int tx_before = hal_tx_starts;
        MbJobContext job2 = {0};
        MbIrTxRawState* s2 = calloc(1, sizeof(MbIrTxRawState));
        job2.module_state = s2;
        CHECK(mb_module_ir_tx_raw.start(&job2, &p) == MB_INIT_FAILED);
        CHECK(hal_tx_starts == tx_before); /* pin failed before worker start */
        pin_fail = false;
        free(s2);
    }
    GROUP("raw tx staging/pin guards");

    /* 10: v2 received hook — fires once on a usable capture,
     * never on over-cap */
    {
        MbJobContext job = {0};
        MbIrRawRxState* s = calloc(1, sizeof(MbIrRawRxState));
        job.module_state = s;
        MbIrRawParams p = {.timeout_ms = 5000};
        CHECK(mb_module_ir_raw_rx.start(&job, &p) == MB_OK);
        uint32_t wave[10] = {0};
        mb_ir_raw_on_timings(wave, 10);
        hook_calls = 0;
        mb_module_ir_raw_rx.service(&job, 100);
        CHECK(job.done && hook_calls == 1);
        CHECK(mb_module_ir_raw_rx.cleanup(&job) == MB_CLEAN_OK);
        free(s);

        MbJobContext job2 = {0};
        MbIrRawRxState* s2 = calloc(1, sizeof(MbIrRawRxState));
        job2.module_state = s2;
        CHECK(mb_module_ir_raw_rx.start(&job2, &p) == MB_OK);
        static uint32_t big[600];
        mb_ir_raw_on_timings(big, 600);
        mb_module_ir_raw_rx.service(&job2, 100);
        CHECK(job2.done && job2.done_status == MB_OVERFLOW);
        CHECK(hook_calls == 1); /* over-cap: no "got it" beep */
        CHECK(mb_module_ir_raw_rx.cleanup(&job2) == MB_CLEAN_OK);
        free(s2);
    }
    GROUP("v2 received hook");

    printf("PASS test_c15r (%d groups)\n", groups);
    return 0;
}
