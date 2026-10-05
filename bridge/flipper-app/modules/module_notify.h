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
