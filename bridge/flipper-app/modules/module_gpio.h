#pragma once
/* Muse Bridge GPIO module (plan section 10.2, checkpoint C10).
 *
 * Portable logic core: no Furi dependency. The FAP binds a HAL table
 * (header pin -> furi_hal_gpio calls) and the claim state; native
 * tests bind a recording fake. Approved general pins are the physical
 * header pins 2..7 (PA7/PA6/PA4/PB3/PB2/PC3 per plan S13). Transport
 * pins (13/14 USART), LPUART (15/16, not yet qualified), iButton (17),
 * 1-Wire SIO (12), SWD (10), power and ground pins are never approved:
 * validation rejects them before any HAL call.
 *
 * A configured pin is a bridge-owned resource that outlives the
 * instantaneous executor job which configured it. Outputs carry a
 * bounded hold deadline; when it passes, the pin returns to the
 * documented P1-A default (analog, no pull) and is unclaimed. A write
 * never refreshes the hold. Lease loss, local exit, and app teardown
 * release every claimed pin through the same restore path.
 */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_GPIO_OP_CONFIG 0x0101
#define MB_GPIO_OP_READ 0x0102
#define MB_GPIO_OP_WRITE 0x0103
#define MB_GPIO_OP_RELEASE 0x0104

#define MB_GPIO_MODE_INPUT 0
#define MB_GPIO_MODE_OUTPUT 1 /* push-pull only in P1-A */

#define MB_GPIO_PULL_NONE 0
#define MB_GPIO_PULL_UP 1
#define MB_GPIO_PULL_DOWN 2

#define MB_GPIO_DEFAULT_HOLD_MS 30000u
#define MB_GPIO_MAX_HOLD_MS 60000u

typedef struct {
    bool claimed;
    uint8_t mode; /* MB_GPIO_MODE_* */
    uint8_t pull; /* MB_GPIO_PULL_* */
    uint8_t value; /* last driven level (outputs) */
    uint32_t deadline_tick; /* outputs: restore at/after; 0 = none */
} MbGpioClaim;

/* HAL indirection: implemented by the FAP glue (furi_hal_gpio) and by
 * native tests (a recorder). mode is MB_GPIO_MODE_*; restore returns
 * the pin to the analog/no-pull default. */
typedef struct {
    void (*apply_config)(void* ctx, uint8_t pin, uint8_t mode, uint8_t pull, uint8_t initial);
    void (*write)(void* ctx, uint8_t pin, uint8_t value);
    bool (*read)(void* ctx, uint8_t pin);
    void (*restore)(void* ctx, uint8_t pin);
    void* ctx;
} MbGpioHal;

typedef struct {
    MbGpioClaim pins[32]; /* indexed by physical header pin */
    uint32_t configs;
    uint32_t writes;
    uint32_t reads;
    uint32_t releases; /* explicit RELEASE actions */
    uint32_t expiries; /* hold-deadline restores */
    uint32_t bulk_releases; /* pins restored by release_all */
} MbGpioState;

/* Executor job parameters for all four GPIO ops (one module). The
 * controller retains this storage until the terminal is acked, as
 * with MbFakeParams. */
typedef struct {
    uint16_t op; /* MB_GPIO_OP_* */
    uint8_t pin;
    uint8_t mode;
    uint8_t pull;
    uint8_t value; /* CONFIG: initial level; WRITE: level */
    uint32_t deadline_tick; /* CONFIG outputs: absolute restore tick
                             * computed by the controller at admission
                             * (start() receives no tick); 0 = none */
} MbGpioParams;

extern const MbModule mb_module_gpio;

/* Bind the state + HAL the executor glue operates on. Called once by
 * the owner (FAP init / native test) before admitting GPIO jobs. */
void mb_gpio_module_bind(MbGpioState* state, const MbGpioHal* hal);
/* Level sampled by the most recent GPIO_READ job (glue-maintained). */
uint8_t mb_gpio_module_last_level(void);

void mb_gpio_init(MbGpioState* state);
bool mb_gpio_pin_approved(uint8_t header_pin);

/* Semantic validation (no HAL touched). Rejection statuses match the
 * C07 admission contract: bad pin/mode/pull/hold/state is consumed
 * MB_INVALID_ARGUMENT; a mode conflict on a claimed pin is consumed
 * MB_RESOURCE_UNAVAILABLE. */
MbStatus mb_gpio_validate_config(
    const MbGpioState* state, uint8_t pin, uint8_t mode, uint8_t pull,
    uint8_t initial, uint32_t hold_ms);
MbStatus mb_gpio_validate_write(const MbGpioState* state, uint8_t pin, uint8_t value);
MbStatus mb_gpio_validate_read(const MbGpioState* state, uint8_t pin);
MbStatus mb_gpio_validate_release(const MbGpioState* state, uint8_t pin);

/* Apply operations (executor thread, under the controller's mutex).
 * Each validates again and mutates only on success. */
MbStatus mb_gpio_apply_config(
    MbGpioState* state, const MbGpioHal* hal, const MbGpioParams* params);
MbStatus mb_gpio_apply_write(MbGpioState* state, const MbGpioHal* hal, uint8_t pin, uint8_t value);
MbStatus mb_gpio_apply_read(MbGpioState* state, const MbGpioHal* hal, uint8_t pin, uint8_t* level_out);
MbStatus mb_gpio_apply_release(MbGpioState* state, const MbGpioHal* hal, uint8_t pin);

/* Hold sweep: restore every output whose deadline has passed.
 * Returns how many pins expired. */
uint32_t mb_gpio_service(MbGpioState* state, const MbGpioHal* hal, uint32_t now);
/* Lease loss / exit / teardown: restore every claimed pin. */
uint32_t mb_gpio_release_all(MbGpioState* state, const MbGpioHal* hal);
uint32_t mb_gpio_claimed_count(const MbGpioState* state);

#ifdef __cplusplus
}
#endif
