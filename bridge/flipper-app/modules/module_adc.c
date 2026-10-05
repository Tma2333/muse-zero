/* Muse Bridge ADC module: channel map, validation, mean-of-samples
 * read. The claim/conflict decision uses the GPIO claim table, which
 * the controller owns; here a claimed pin is a hard refusal. */
#include "module_adc.h"
#include "module_gpio.h"

#include <string.h>

void mb_adc_init(MbAdcState* state) {
    memset(state, 0, sizeof(*state));
    state->channel = 0xFF;
}

uint8_t mb_adc_channel_for_pin(uint8_t header_pin) {
    switch(header_pin) {
    case 2:
        return 12; /* PA7 */
    case 3:
        return 11; /* PA6 */
    case 4:
        return 9; /* PA4 */
    case 7:
        return 4; /* PC3 */
    default:
        return 0xFF; /* includes 5/PB3 and 6/PB2: no ADC */
    }
}

MbStatus mb_adc_validate(uint8_t pin, uint8_t samples, bool pin_claimed) {
    if(samples < 1 || samples > MB_ADC_SAMPLES_MAX) return MB_INVALID_ARGUMENT;
    if(!mb_gpio_pin_approved(pin)) return MB_INVALID_ARGUMENT;
    if(mb_adc_channel_for_pin(pin) == 0xFF) return MB_INVALID_ARGUMENT;
    if(pin_claimed) return MB_RESOURCE_UNAVAILABLE;
    return MB_OK;
}

MbStatus mb_adc_apply_read(MbAdcState* state, const MbAdcHal* hal, const MbAdcParams* params) {
    uint8_t channel = mb_adc_channel_for_pin(params->pin);
    if(channel == 0xFF || params->samples < 1 || params->samples > MB_ADC_SAMPLES_MAX) {
        return MB_INVALID_ARGUMENT;
    }
    if(!hal->prepare_pin(hal->ctx, params->pin)) return MB_IO_ERROR;
    uint32_t raw_sum = 0, mv_sum = 0;
    MbStatus out = MB_OK;
    for(uint8_t i = 0; i < params->samples; i++) {
        uint16_t raw = 0, mv = 0;
        if(!hal->read_sample(hal->ctx, channel, &raw, &mv)) {
            out = MB_IO_ERROR;
            break;
        }
        raw_sum += raw;
        mv_sum += mv;
        state->samples_total++;
    }
    hal->restore_pin(hal->ctx, params->pin);
    if(out == MB_OK) {
        state->channel = channel;
        state->raw_mean = (uint16_t)(raw_sum / params->samples);
        state->mv_mean = (uint16_t)(mv_sum / params->samples);
        state->reads++;
    }
    return out;
}

/* ---- Executor glue (binds like module_gpio). */
static MbAdcState* adc_bound_state;
static MbAdcHal adc_bound_hal;
static bool adc_bound;

void mb_adc_module_bind(MbAdcState* state, const MbAdcHal* hal) {
    adc_bound_state = state;
    if(hal != NULL) adc_bound_hal = *hal;
    adc_bound = state != NULL && hal != NULL;
}

static MbStatus adc_validate_fn(const void* params) {
    const MbAdcParams* p = params;
    if(p == NULL || !adc_bound) return MB_INVALID_ARGUMENT;
    if(mb_adc_channel_for_pin(p->pin) == 0xFF) return MB_INVALID_ARGUMENT;
    if(p->samples < 1 || p->samples > MB_ADC_SAMPLES_MAX) return MB_INVALID_ARGUMENT;
    return MB_OK;
}

static MbStatus adc_start(MbJobContext* job, const void* params) {
    (void)job;
    if(!adc_bound) return MB_INIT_FAILED;
    return mb_adc_apply_read(adc_bound_state, &adc_bound_hal, params);
}

static void adc_service(MbJobContext* job, uint32_t now) {
    (void)now;
    job->done = true;
    job->done_status = MB_OK;
}

static void adc_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)job;
    (void)reason;
}

static MbCleanupResult adc_cleanup(MbJobContext* job) {
    (void)job;
    return MB_CLEAN_OK;
}

const MbModule mb_module_adc = {
    .validate = adc_validate_fn,
    .ctx_size = 0,
    .start = adc_start,
    .service = adc_service,
    .request_stop = adc_request_stop,
    .cleanup = adc_cleanup,
};
