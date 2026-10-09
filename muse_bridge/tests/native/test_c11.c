/* C11 native tests: ADC channel map/validation/mean logic and NOTIFY
 * effect validation against a scripted fake HAL. Invalid requests
 * must produce zero HAL calls. */
#include <stdio.h>
#include <string.h>

#include "../../modules/module_adc.h"
#include "../../modules/module_notify.h"

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

/* ---- Fake ADC HAL: scripted raw/mv sequences ---- */
typedef struct {
    int prepares, restores, reads;
    const uint16_t* raws;
    const uint16_t* mvs;
    int n, idx, fail_at; /* fail_at: read index to fail, -1 = never */
} FakeAdc;

static bool adc_prepare(void* ctx, uint8_t pin) {
    (void)pin;
    ((FakeAdc*)ctx)->prepares++;
    return true;
}

static void adc_restore(void* ctx, uint8_t pin) {
    (void)pin;
    ((FakeAdc*)ctx)->restores++;
}

static bool adc_read(void* ctx, uint8_t channel, uint16_t* raw, uint16_t* mv) {
    FakeAdc* f = ctx;
    (void)channel;
    if(f->idx == f->fail_at) return false;
    *raw = f->raws[f->idx % f->n];
    *mv = f->mvs[f->idx % f->n];
    f->idx++;
    f->reads++;
    return true;
}

/* ---- Fake NOTIFY HAL ---- */
typedef struct {
    int plays;
    uint8_t last;
} FakeNotify;

static void notify_play(void* ctx, uint8_t effect) {
    FakeNotify* f = ctx;
    f->plays++;
    f->last = effect;
}

int main(void) {
    /* 1: channel map (plan 10.2) */
    CHECK(mb_adc_channel_for_pin(2) == 12);
    CHECK(mb_adc_channel_for_pin(3) == 11);
    CHECK(mb_adc_channel_for_pin(4) == 9);
    CHECK(mb_adc_channel_for_pin(7) == 4);
    CHECK(mb_adc_channel_for_pin(5) == 0xFF); /* PB3: digital only */
    CHECK(mb_adc_channel_for_pin(6) == 0xFF); /* PB2: digital only */
    CHECK(mb_adc_channel_for_pin(13) == 0xFF);
    CHECK(mb_adc_channel_for_pin(0) == 0xFF);
    GROUP("adc channel map");

    /* 2: validation matrix */
    CHECK(mb_adc_validate(2, 1, false) == MB_OK);
    CHECK(mb_adc_validate(2, 16, false) == MB_OK);
    CHECK(mb_adc_validate(2, 0, false) == MB_INVALID_ARGUMENT);
    CHECK(mb_adc_validate(2, 17, false) == MB_INVALID_ARGUMENT);
    CHECK(mb_adc_validate(5, 4, false) == MB_INVALID_ARGUMENT); /* non-ADC pin */
    CHECK(mb_adc_validate(13, 4, false) == MB_INVALID_ARGUMENT); /* reserved */
    CHECK(mb_adc_validate(1, 4, false) == MB_INVALID_ARGUMENT);
    CHECK(mb_adc_validate(2, 4, true) == MB_RESOURCE_UNAVAILABLE); /* GPIO claim */
    GROUP("adc validation");

    /* 3: mean-of-samples read; pin prepared and restored exactly once */
    MbAdcState as;
    mb_adc_init(&as);
    static const uint16_t raws[4] = {100, 200, 300, 400};
    static const uint16_t mvs[4] = {50, 100, 150, 200};
    FakeAdc fa = {.raws = raws, .mvs = mvs, .n = 4, .fail_at = -1};
    MbAdcHal ahal = {
        .prepare_pin = adc_prepare, .restore_pin = adc_restore,
        .read_sample = adc_read, .ctx = &fa};
    MbAdcParams rp = {.pin = 2, .samples = 4};
    CHECK(mb_adc_apply_read(&as, &ahal, &rp) == MB_OK);
    CHECK(fa.prepares == 1 && fa.restores == 1 && fa.reads == 4);
    CHECK(as.channel == 12 && as.raw_mean == 250 && as.mv_mean == 125);
    CHECK(as.reads == 1 && as.samples_total == 4);
    GROUP("adc mean read");

    /* 4: read failure still restores the pin, records nothing */
    FakeAdc fb = {.raws = raws, .mvs = mvs, .n = 4, .fail_at = 2};
    ahal.ctx = &fb;
    MbAdcState as2;
    mb_adc_init(&as2);
    CHECK(mb_adc_apply_read(&as2, &ahal, &rp) == MB_IO_ERROR);
    CHECK(fb.prepares == 1 && fb.restores == 1 && fb.reads == 2);
    CHECK(as2.reads == 0);
    GROUP("adc failure restores");

    /* 5: notify validation + play */
    CHECK(mb_notify_validate(0) == MB_INVALID_ARGUMENT);
    CHECK(mb_notify_validate(1) == MB_OK);
    CHECK(mb_notify_validate(2) == MB_OK);
    CHECK(mb_notify_validate(3) == MB_OK);
    CHECK(mb_notify_validate(4) == MB_OK);
    CHECK(mb_notify_validate(5) == MB_OK);
    CHECK(mb_notify_validate(8) == MB_OK);
    CHECK(mb_notify_validate(9) == MB_INVALID_ARGUMENT);
    MbNotifyState ns;
    mb_notify_init(&ns);
    FakeNotify fn = {0};
    MbNotifyHal nhal = {.play = notify_play, .ctx = &fn};
    CHECK(mb_notify_apply(&ns, &nhal, 9) == MB_INVALID_ARGUMENT);
    CHECK(fn.plays == 0);
    CHECK(mb_notify_apply(&ns, &nhal, MB_NOTIFY_SHORT_BEEP) == MB_OK);
    CHECK(fn.plays == 1 && fn.last == 2 && ns.effects_played == 1);
    GROUP("notify validation and play");

    /* 6: executor glue */
    mb_adc_module_bind(&as, &ahal);
    MbAdcParams bad = {.pin = 5, .samples = 4};
    CHECK(mb_module_adc.validate(&bad) == MB_INVALID_ARGUMENT);
    bad.pin = 2;
    bad.samples = 17;
    CHECK(mb_module_adc.validate(&bad) == MB_INVALID_ARGUMENT);
    mb_notify_module_bind(&ns, &nhal);
    MbNotifyParams np = {.effect = 1};
    MbJobContext ctx = {0};
    CHECK(mb_module_notify.validate(&np) == MB_OK);
    CHECK(mb_module_notify.start(&ctx, &np) == MB_OK);
    CHECK(fn.plays == 2 && fn.last == 1);
    mb_module_notify.service(&ctx, 0);
    CHECK(ctx.done && ctx.done_status == MB_OK);
    GROUP("executor glue");

    printf("test_c11: all %d groups passed\n", groups);
    return 0;
}
