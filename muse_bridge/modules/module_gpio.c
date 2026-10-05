/* Muse Bridge GPIO module: portable claim logic + executor glue.
 * See module_gpio.h. The claim table lives outside the executor job
 * slot: configuring a pin is an instantaneous job, the claim is a
 * resource released only by GPIO_RELEASE, hold expiry, lease loss, or
 * app teardown. */
#include "module_gpio.h"

#include <string.h>

bool mb_gpio_pin_approved(uint8_t header_pin) {
    /* Plan 10.2 allowlist (P1-A): PA7/PA6/PA4/PB3/PB2/PC3 only. */
    return header_pin >= 2 && header_pin <= 7;
}

void mb_gpio_init(MbGpioState* state) {
    memset(state, 0, sizeof(*state));
}

static bool claim_index_ok(uint8_t pin) {
    return pin < 32;
}

MbStatus mb_gpio_validate_config(
    const MbGpioState* state, uint8_t pin, uint8_t mode, uint8_t pull,
    uint8_t initial, uint32_t hold_ms) {
    if(!mb_gpio_pin_approved(pin)) return MB_INVALID_ARGUMENT;
    if(mode > MB_GPIO_MODE_OUTPUT || pull > MB_GPIO_PULL_DOWN) return MB_INVALID_ARGUMENT;
    if(initial > 1) return MB_INVALID_ARGUMENT;
    if(mode == MB_GPIO_MODE_OUTPUT) {
        if(pull != MB_GPIO_PULL_NONE) return MB_INVALID_ARGUMENT;
        if(hold_ms > MB_GPIO_MAX_HOLD_MS) return MB_INVALID_ARGUMENT;
    } else {
        /* Inputs have no output hold and no initial level. */
        if(initial != 0 || hold_ms != 0) return MB_INVALID_ARGUMENT;
    }
    if(claim_index_ok(pin) && state->pins[pin].claimed && state->pins[pin].mode != mode) {
        return MB_RESOURCE_UNAVAILABLE; /* conflicting mode: no register touched */
    }
    return MB_OK;
}

MbStatus mb_gpio_validate_write(const MbGpioState* state, uint8_t pin, uint8_t value) {
    if(value > 1) return MB_INVALID_ARGUMENT;
    if(!claim_index_ok(pin) || !state->pins[pin].claimed ||
       state->pins[pin].mode != MB_GPIO_MODE_OUTPUT) {
        return MB_INVALID_ARGUMENT;
    }
    return MB_OK;
}

MbStatus mb_gpio_validate_read(const MbGpioState* state, uint8_t pin) {
    if(!claim_index_ok(pin) || !state->pins[pin].claimed ||
       state->pins[pin].mode != MB_GPIO_MODE_INPUT) {
        return MB_INVALID_ARGUMENT;
    }
    return MB_OK;
}

MbStatus mb_gpio_validate_release(const MbGpioState* state, uint8_t pin) {
    if(!claim_index_ok(pin) || !state->pins[pin].claimed) return MB_INVALID_ARGUMENT;
    return MB_OK;
}

MbStatus mb_gpio_apply_config(MbGpioState* state, const MbGpioHal* hal, const MbGpioParams* params) {
    /* Milliseconds were folded into the absolute deadline by the
     * controller at admission; re-validate the wire semantics. */
    if(!mb_gpio_pin_approved(params->pin)) return MB_INVALID_ARGUMENT;
    if(params->mode > MB_GPIO_MODE_OUTPUT || params->pull > MB_GPIO_PULL_DOWN) {
        return MB_INVALID_ARGUMENT;
    }
    MbGpioClaim* c = &state->pins[params->pin];
    if(c->claimed && c->mode != params->mode) return MB_RESOURCE_UNAVAILABLE;
    hal->apply_config(hal->ctx, params->pin, params->mode, params->pull, params->value);
    c->claimed = true;
    c->mode = params->mode;
    c->pull = params->pull;
    c->value = params->mode == MB_GPIO_MODE_OUTPUT ? params->value : 0;
    /* Reconfiguration under a new action starts a new bounded hold. */
    c->deadline_tick = params->mode == MB_GPIO_MODE_OUTPUT ? params->deadline_tick : 0;
    state->configs++;
    return MB_OK;
}

MbStatus mb_gpio_apply_write(MbGpioState* state, const MbGpioHal* hal, uint8_t pin, uint8_t value) {
    MbStatus v = mb_gpio_validate_write(state, pin, value);
    if(v != MB_OK) return v;
    hal->write(hal->ctx, pin, value);
    state->pins[pin].value = value; /* the hold deadline is NOT refreshed */
    state->writes++;
    return MB_OK;
}

MbStatus mb_gpio_apply_read(MbGpioState* state, const MbGpioHal* hal, uint8_t pin, uint8_t* level_out) {
    MbStatus v = mb_gpio_validate_read(state, pin);
    if(v != MB_OK) return v;
    *level_out = hal->read(hal->ctx, pin) ? 1 : 0;
    state->reads++;
    return MB_OK;
}

MbStatus mb_gpio_apply_release(MbGpioState* state, const MbGpioHal* hal, uint8_t pin) {
    MbStatus v = mb_gpio_validate_release(state, pin);
    if(v != MB_OK) return v;
    hal->restore(hal->ctx, pin);
    memset(&state->pins[pin], 0, sizeof(MbGpioClaim));
    state->releases++;
    return MB_OK;
}

uint32_t mb_gpio_service(MbGpioState* state, const MbGpioHal* hal, uint32_t now) {
    uint32_t expired = 0;
    for(uint8_t pin = 0; pin < 32; pin++) {
        MbGpioClaim* c = &state->pins[pin];
        if(!c->claimed || c->mode != MB_GPIO_MODE_OUTPUT || c->deadline_tick == 0) continue;
        if((int32_t)(now - c->deadline_tick) >= 0) {
            hal->restore(hal->ctx, pin);
            memset(c, 0, sizeof(*c));
            state->expiries++;
            expired++;
        }
    }
    return expired;
}

uint32_t mb_gpio_release_all(MbGpioState* state, const MbGpioHal* hal) {
    uint32_t released = 0;
    for(uint8_t pin = 0; pin < 32; pin++) {
        if(state->pins[pin].claimed) {
            hal->restore(hal->ctx, pin);
            memset(&state->pins[pin], 0, sizeof(MbGpioClaim));
            state->bulk_releases++;
            released++;
        }
    }
    return released;
}

uint32_t mb_gpio_claimed_count(const MbGpioState* state) {
    uint32_t n = 0;
    for(uint8_t pin = 0; pin < 32; pin++) {
        if(state->pins[pin].claimed) n++;
    }
    return n;
}

/* ---- Executor glue (one module for all four GPIO ops). The bound
 * state outlives individual jobs, so ctx_size is zero. */
static MbGpioState* gpio_bound_state;
static MbGpioHal gpio_bound_hal;
static bool gpio_bound;
static uint8_t gpio_last_level;

void mb_gpio_module_bind(MbGpioState* state, const MbGpioHal* hal) {
    gpio_bound_state = state;
    if(hal != NULL) gpio_bound_hal = *hal;
    gpio_bound = state != NULL && hal != NULL;
}

uint8_t mb_gpio_module_last_level(void) {
    return gpio_last_level;
}

static MbStatus gpio_validate(const void* params) {
    const MbGpioParams* p = params;
    if(p == NULL || !gpio_bound) return MB_INVALID_ARGUMENT;
    if(p->op < MB_GPIO_OP_CONFIG || p->op > MB_GPIO_OP_RELEASE) return MB_INVALID_ARGUMENT;
    if(!mb_gpio_pin_approved(p->pin)) return MB_INVALID_ARGUMENT;
    return MB_OK;
}

static MbStatus gpio_start(MbJobContext* job, const void* params) {
    (void)job;
    const MbGpioParams* p = params;
    if(!gpio_bound) return MB_INIT_FAILED;
    switch(p->op) {
    case MB_GPIO_OP_CONFIG:
        return mb_gpio_apply_config(gpio_bound_state, &gpio_bound_hal, p);
    case MB_GPIO_OP_WRITE:
        return mb_gpio_apply_write(gpio_bound_state, &gpio_bound_hal, p->pin, p->value);
    case MB_GPIO_OP_READ: {
        uint8_t level = 0;
        MbStatus s = mb_gpio_apply_read(gpio_bound_state, &gpio_bound_hal, p->pin, &level);
        if(s == MB_OK) gpio_last_level = level;
        return s;
    }
    case MB_GPIO_OP_RELEASE:
        return mb_gpio_apply_release(gpio_bound_state, &gpio_bound_hal, p->pin);
    default:
        return MB_INVALID_ARGUMENT;
    }
}

static void gpio_service(MbJobContext* job, uint32_t now) {
    (void)now;
    job->done = true; /* the op completed synchronously in start() */
    job->done_status = MB_OK;
}

static void gpio_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)job;
    (void)reason;
}

static MbCleanupResult gpio_cleanup(MbJobContext* job) {
    (void)job;
    return MB_CLEAN_OK; /* claims are resources; they persist by design */
}

const MbModule mb_module_gpio = {
    .validate = gpio_validate,
    .ctx_size = 0,
    .start = gpio_start,
    .service = gpio_service,
    .request_stop = gpio_request_stop,
    .cleanup = gpio_cleanup,
};
