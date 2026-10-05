/* C10 native tests: GPIO claim logic against a recording fake HAL.
 * The point of the module is that invalid requests never touch the
 * hardware: every rejection is asserted with a zero HAL-call delta. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../modules/module_gpio.h"

typedef struct {
    int configs[32];
    uint8_t mode[32], pull[32], initial[32];
    int writes[32];
    uint8_t value[32];
    int reads[32];
    bool level[32];
    int restores[32];
    int total;
} FakeHal;

static void hal_config(void* ctx, uint8_t pin, uint8_t mode, uint8_t pull, uint8_t initial) {
    FakeHal* h = ctx;
    h->configs[pin]++;
    h->mode[pin] = mode;
    h->pull[pin] = pull;
    h->initial[pin] = initial;
    h->total++;
}

static void hal_write(void* ctx, uint8_t pin, uint8_t value) {
    FakeHal* h = ctx;
    h->writes[pin]++;
    h->value[pin] = value;
    h->total++;
}

static bool hal_read(void* ctx, uint8_t pin) {
    FakeHal* h = ctx;
    h->reads[pin]++;
    h->total++;
    return h->level[pin];
}

static void hal_restore(void* ctx, uint8_t pin) {
    FakeHal* h = ctx;
    h->restores[pin]++;
    h->total++;
}

static int groups;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while(0)
#define GROUP(name) do { groups++; printf("ok %d - %s\n", groups, name); } while(0)

static MbGpioParams cfg(uint8_t pin, uint8_t mode, uint8_t pull, uint8_t initial, uint32_t deadline) {
    MbGpioParams p = {0};
    p.op = MB_GPIO_OP_CONFIG;
    p.pin = pin;
    p.mode = mode;
    p.pull = pull;
    p.value = initial;
    p.deadline_tick = deadline;
    return p;
}

int main(void) {
    FakeHal h;
    MbGpioHal hal = {
        .apply_config = hal_config, .write = hal_write, .read = hal_read,
        .restore = hal_restore, .ctx = &h};
    MbGpioState s;
    memset(&h, 0, sizeof(h));
    mb_gpio_init(&s);

    /* 1: allowlist is exactly header pins 2..7. */
    for(int p = 0; p < 256; p++) {
        bool want = p >= 2 && p <= 7;
        CHECK(mb_gpio_pin_approved((uint8_t)p) == want);
    }
    GROUP("allowlist pins 2..7 only");

    /* 2: validation matrix — no HAL call may result from any of it. */
    int before = h.total;
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 30000) == MB_OK);
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 0, 0) == MB_OK);
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 60000) == MB_OK);
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 60001) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_UP, 1, 1000) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 3, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_UP, 0, 0) == MB_OK);
    CHECK(mb_gpio_validate_config(&s, 3, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_DOWN, 0, 0) == MB_OK);
    CHECK(mb_gpio_validate_config(&s, 3, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_NONE, 1, 0) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 3, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_NONE, 0, 5) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 2, 2, 0, 0, 0) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 2, 0, 3, 0, 0) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_validate_config(&s, 2, 1, 0, 2, 0) == MB_INVALID_ARGUMENT);
    const uint8_t bad_pins[] = {0, 1, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 31, 255};
    for(size_t i = 0; i < sizeof(bad_pins); i++) {
        CHECK(mb_gpio_validate_config(&s, bad_pins[i], 1, 0, 1, 1000) == MB_INVALID_ARGUMENT);
        CHECK(mb_gpio_validate_read(&s, bad_pins[i]) == MB_INVALID_ARGUMENT);
        CHECK(mb_gpio_validate_release(&s, bad_pins[i]) == MB_INVALID_ARGUMENT);
    }
    CHECK(h.total == before);
    GROUP("validation matrix, zero HAL calls");

    /* 3: apply config (output, deadline 5000) + write keeps deadline. */
    MbGpioParams out2 = cfg(2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 5000);
    CHECK(mb_gpio_apply_config(&s, &hal, &out2) == MB_OK);
    CHECK(h.configs[2] == 1 && h.mode[2] == MB_GPIO_MODE_OUTPUT && h.initial[2] == 1);
    CHECK(s.pins[2].claimed && s.pins[2].deadline_tick == 5000 && s.pins[2].value == 1);
    CHECK(mb_gpio_apply_write(&s, &hal, 2, 0) == MB_OK);
    CHECK(h.writes[2] == 1 && h.value[2] == 0);
    CHECK(s.pins[2].deadline_tick == 5000); /* NOT refreshed */
    CHECK(mb_gpio_apply_write(&s, &hal, 2, 2) == MB_INVALID_ARGUMENT);
    CHECK(mb_gpio_apply_write(&s, &hal, 4, 1) == MB_INVALID_ARGUMENT); /* unclaimed */
    GROUP("config output + write without hold refresh");

    /* 4: mode conflict refuses before any register write. */
    before = h.total;
    MbGpioParams as_input = cfg(2, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_NONE, 0, 0);
    CHECK(mb_gpio_validate_config(&s, 2, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_NONE, 0, 0) ==
          MB_RESOURCE_UNAVAILABLE);
    CHECK(mb_gpio_apply_config(&s, &hal, &as_input) == MB_RESOURCE_UNAVAILABLE);
    CHECK(h.total == before);
    /* Same-mode reconfig is admitted and starts a NEW hold. */
    MbGpioParams re = cfg(2, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 0, 9000);
    CHECK(mb_gpio_apply_config(&s, &hal, &re) == MB_OK);
    CHECK(s.pins[2].deadline_tick == 9000 && s.pins[2].value == 0);
    GROUP("mode conflict rejected, same-mode reconfig re-holds");

    /* 5: hold expiry restores exactly at the deadline; inputs never expire. */
    MbGpioParams in3 = cfg(3, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_DOWN, 0, 0);
    CHECK(mb_gpio_apply_config(&s, &hal, &in3) == MB_OK);
    CHECK(mb_gpio_service(&s, &hal, 8999) == 0);
    CHECK(s.pins[2].claimed);
    CHECK(mb_gpio_service(&s, &hal, 9000) == 1);
    CHECK(!s.pins[2].claimed && h.restores[2] == 1 && s.expiries == 1);
    CHECK(mb_gpio_service(&s, &hal, 0x7FFFFFF0u) == 0);
    CHECK(s.pins[3].claimed); /* input claim persists */
    GROUP("hold expiry restores output, input persists");

    /* 6: read semantics. */
    h.level[3] = true;
    uint8_t level = 0xEE;
    CHECK(mb_gpio_apply_read(&s, &hal, 3, &level) == MB_OK && level == 1);
    h.level[3] = false;
    CHECK(mb_gpio_apply_read(&s, &hal, 3, &level) == MB_OK && level == 0);
    CHECK(mb_gpio_apply_read(&s, &hal, 4, &level) == MB_INVALID_ARGUMENT);
    GROUP("read claimed input only");

    /* 7: release restores once; a second release is refused. */
    before = h.total;
    CHECK(mb_gpio_apply_release(&s, &hal, 3) == MB_OK);
    CHECK(!s.pins[3].claimed && s.releases == 1);
    CHECK(mb_gpio_apply_release(&s, &hal, 3) == MB_INVALID_ARGUMENT);
    CHECK(h.total == before + 1);
    GROUP("release restores exactly once");

    /* 8: release_all restores every claim (lease-loss path). */
    MbGpioParams o4 = cfg(4, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 60000);
    MbGpioParams i5 = cfg(5, MB_GPIO_MODE_INPUT, MB_GPIO_PULL_UP, 0, 0);
    CHECK(mb_gpio_apply_config(&s, &hal, &o4) == MB_OK);
    CHECK(mb_gpio_apply_config(&s, &hal, &i5) == MB_OK);
    CHECK(mb_gpio_claimed_count(&s) == 2);
    CHECK(mb_gpio_release_all(&s, &hal) == 2);
    CHECK(mb_gpio_claimed_count(&s) == 0 && s.bulk_releases == 2);
    GROUP("release_all restores all claims");

    /* 9: executor glue performs the op and reports the sampled level. */
    mb_gpio_module_bind(&s, &hal);
    MbGpioParams job = cfg(6, MB_GPIO_MODE_OUTPUT, MB_GPIO_PULL_NONE, 1, 1234);
    MbJobContext ctx = {0};
    CHECK(mb_module_gpio.validate(&job) == MB_OK);
    job.op = 0x0105;
    CHECK(mb_module_gpio.validate(&job) == MB_INVALID_ARGUMENT);
    job.op = MB_GPIO_OP_CONFIG;
    job.pin = 13;
    CHECK(mb_module_gpio.validate(&job) == MB_INVALID_ARGUMENT);
    job.pin = 6;
    CHECK(mb_module_gpio.start(&ctx, &job) == MB_OK);
    CHECK(s.pins[6].claimed && s.pins[6].deadline_tick == 1234);
    mb_module_gpio.service(&ctx, 0);
    CHECK(ctx.done && ctx.done_status == MB_OK);
    CHECK(mb_module_gpio.cleanup(&ctx) == MB_CLEAN_OK);
    CHECK(s.pins[6].claimed); /* cleanup keeps the claim by design */
    GROUP("executor glue start/service/cleanup");

    printf("test_c10: all %d groups passed\n", groups);
    return 0;
}
