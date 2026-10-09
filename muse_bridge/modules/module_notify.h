#pragma once
/* Muse Bridge notification module (plan section 10.4, checkpoint
 * C11). A small set of named, finite effects run as short executor
 * jobs (at most 250 ms each); no arbitrary sequence upload exists.
 * Portable logic: effect validation and counters; the FAP binds the
 * notification-service call (notification_message_block with static
 * sequence storage) which runs on the executor thread only.
 */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_NOTIFY_OP 0x0301
#define MB_NOTIFY_GREEN_FLASH 1
#define MB_NOTIFY_SHORT_BEEP 2
#define MB_NOTIFY_SHORT_VIBRATION 3
/* Generalized bench signals (2026-10-08, operator-designed): the
 * device itself announces test/listening state, so a human at the
 * bench never has to guess timing from chat messages. DOUBLE_BEEP
 * is the start/end marker (fits the 250 ms bound: 100+50+100).
 * LISTEN_ON/OFF drive the app's listening indicator (flashing blue
 * LED while any hardware-listening job or host request says the
 * device is listening). SIGNALS_OFF/ON mute/unmute the AUTOMATIC
 * job signals so long cycle tests stay quiet; explicit effects
 * always play. */
#define MB_NOTIFY_DOUBLE_BEEP 4
#define MB_NOTIFY_LISTEN_ON 5
#define MB_NOTIFY_LISTEN_OFF 6
#define MB_NOTIFY_SIGNALS_OFF 7
#define MB_NOTIFY_SIGNALS_ON 8
#define MB_NOTIFY_MAX_EFFECT_MS 250

typedef struct {
    void (*play)(void* ctx, uint8_t effect);
    void* ctx;
} MbNotifyHal;

typedef struct {
    uint32_t effects_played;
    uint8_t last_effect;
} MbNotifyState;

typedef struct {
    uint8_t effect;
} MbNotifyParams;

extern const MbModule mb_module_notify;

void mb_notify_init(MbNotifyState* state);
void mb_notify_module_bind(MbNotifyState* state, const MbNotifyHal* hal);

MbStatus mb_notify_validate(uint8_t effect);
MbStatus mb_notify_apply(MbNotifyState* state, const MbNotifyHal* hal, uint8_t effect);

#ifdef __cplusplus
}
#endif
