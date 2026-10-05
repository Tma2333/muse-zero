#pragma once
/* Muse Bridge ADC module (plan section 10.3, checkpoint C11).
 *
 * Portable logic: pin->channel map and validation; the FAP binds the
 * furi_hal_adc implementation. ADC_READ is an instantaneous executor
 * job on an unclaimed, approved, ADC-capable pin: the job puts the
 * pin in analog mode, takes 1..16 samples on the default 0-2048 mV
 * scale, and the pin ends back at the analog default, unclaimed.
 * Internal channels (VREFINT/temp/VBAT) are never exposed.
 */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_ADC_OP_READ 0x0201
#define MB_ADC_SAMPLES_MAX 16
#define MB_ADC_SCALE_MV 2048 /* advertised default scale */

typedef struct {
    bool (*prepare_pin)(void* ctx, uint8_t pin); /* analog mode */
    void (*restore_pin)(void* ctx, uint8_t pin); /* analog default */
    bool (*read_sample)(void* ctx, uint8_t channel, uint16_t* raw, uint16_t* mv);
    void* ctx;
} MbAdcHal;

typedef struct {
    uint32_t reads;
    uint32_t samples_total;
    /* outcome of the most recent read (glue-maintained) */
    uint8_t channel;
    uint16_t raw_mean;
    uint16_t mv_mean;
} MbAdcState;

typedef struct {
    uint8_t pin;
    uint8_t samples;
} MbAdcParams;

extern const MbModule mb_module_adc;

void mb_adc_init(MbAdcState* state);
void mb_adc_module_bind(MbAdcState* state, const MbAdcHal* hal);

/* ADC channel for a header pin, or 0xFF if it has none (plan 10.2:
 * pin 2=PA7/ch12, 3=PA6/ch11, 4=PA4/ch9, 7=PC3/ch4; 5/6 have none). */
uint8_t mb_adc_channel_for_pin(uint8_t header_pin);

/* pin_claimed: the GPIO module owns the pin right now (conflict). */
MbStatus mb_adc_validate(uint8_t pin, uint8_t samples, bool pin_claimed);

MbStatus mb_adc_apply_read(
    MbAdcState* state, const MbAdcHal* hal, const MbAdcParams* params);

#ifdef __cplusplus
}
#endif
