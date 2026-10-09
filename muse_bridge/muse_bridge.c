/*
 * Muse Bridge — M0 shell + M1/C04 codec + C05 executor.
 *
 * Takes the raw USART from the expansion service (expansion_disable ->
 * serial acquire, per expansion.h), frames Pi traffic with the COBS/CRC
 * fallback protocol (plan §6.3/§6.5), answers bootstrap PING/GET_INFO,
 * and restores everything on exit (release -> expansion_enable).
 * The executor thread runs the one-job slot (plan §5.1/§9); in this
 * qualification build it executes a single internally-triggered fake
 * job shortly after launch. Fake actions are NOT reachable from the
 * wire; sessions/ledger arrive in M2, hardware modules in M3+.
 *
 * C10 adds the first real hardware module: GPIO config/read/write/
 * release on allowlisted header pins 2..7, plus GET_CAPABILITIES.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_random.h>
#include <furi_hal_serial.h>
#include <furi_hal_resources.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <expansion/expansion.h>
#include <storage/storage.h>
#include <furi_hal_adc.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <infrared_worker.h>

#include "core/bridge_core.h"
#include "core/bridge_diag.h"
#include "core/bridge_executor.h"
#include "core/bridge_session.h"
#include "transport/bridge_codec.h"
#include "modules/module_fake.h"
#include "core/bridge_build_config.h"
#include "modules/module_gpio.h"
#include "modules/module_adc.h"
#include "modules/module_notify.h"
#include "modules/module_ir.h"
#include "modules/module_object.h"

#if MB_RELEASE_BUILD
#define BRIDGE_BUILD_ID "mb-1.0-rel-1"
#else
#define BRIDGE_BUILD_ID "mb-0.12-c15-2"
#endif
#define BRIDGE_BAUD 230400
#define BRIDGE_AUTO_EXIT_MS (600U * 1000U) /* dev-only bench safety net (compiled out of release); 600 s since C15 — witness-judged cases run minutes */
#define BRIDGE_PARTIAL_MS 250U

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    volatile bool exit_requested;
    volatile bool uart_acquired;
    volatile bool ring_overflowed;
    volatile uint32_t rx_bytes;
    volatile uint32_t rx_errors;
    volatile uint32_t ring_drops;
    uint8_t boot_id[16];
    uint32_t started_tick;
    MbRx rx;
    uint8_t decoded[MB_FRAME_MAX];
    uint32_t valid_frames;
    uint32_t crc_errors;
    uint32_t oversize_frames;
    uint32_t partial_timeouts;
    uint32_t codec_errors;
    uint32_t unsupported;
    /* C05 executor (executor thread owns the job lifecycle; every API
     * call is serialized through exec_mutex in this integration). */
    MbExecutor exec;
    MbAlloc exec_alloc;
    FuriMutex* exec_mutex;
    FuriThread* exec_thread;
    volatile bool exec_thread_stop;
    volatile uint32_t exec_heartbeats;
    MbFakeParams fake_params;
    /* C10 GPIO: the claim table (a configured pin is a resource that
     * outlives the instantaneous job which claimed it), its HAL, and
     * the retained params of the in-flight GPIO job. */
    MbGpioState gpio_state;
    MbGpioHal gpio_hal;
    MbGpioParams gpio_params;
    /* C11 ADC + notifications: instantaneous/finite modules on the
     * same executor slot. */
    MbAdcState adc_state;
    MbAdcHal adc_hal;
    MbAdcParams adc_params;
    MbNotifyState notify_state;
    MbNotifyHal notify_hal;
    MbNotifyParams notify_params;
    NotificationApp* notification;
    /* C12 IR receive: async worker module. */
    MbIrParams ir_params;
    MbIrTxParams ir_tx_params;
    MbIrHal ir_hal;
    InfraredWorker* ir_worker;
    /* C15 raw IR + bounded object store. */
    MbIrRawParams raw_rx_params;
    MbIrTxRawParams raw_tx_params;
    MbObjParams obj_params;
    MbObjectStore objects;
    MbIdentity obj_seen_identity;
    bool obj_seen_valid;
    /* Bench signals: signal_listen is the truth (a hardware-
     * listening job or a host LISTEN_ON is active); the main loop
     * flashes the LED while it is set. signal_muted silences the
     * AUTOMATIC beeps/blinks for long cycle tests. */
    bool signal_listen;
    bool signal_muted;
    uint32_t signal_last_blink;
    /* C07 wire job: the executor's current action sequence (0 = none)
     * and the executor counters at admission, for the terminal summary. */
    uint64_t active_seq;
    uint16_t active_op; /* C10: the in-flight job's wire op (COMPLETE/EVENT) */
    uint32_t admit_generated, admit_dropped;
    bool back_pressed;
    bool stop_sent;
    uint32_t cancel_requests; /* C08: wire CANCELs that latched a stop */
    uint32_t fake_terminals;
    uint32_t fake_last_status;
    /* heap baseline (C01): free/min/max-block after setup and after
     * teardown, recorded per launch for drift comparison. */
    size_t heap_start_free, heap_start_min, heap_start_block;
    size_t heap_end_free, heap_end_min, heap_end_block;
    /* C02 bounded trace ring (no wire access yet). */
    MbDiag diag;
    /* rx noise timing probe: counters snapshotted at ~1 s so a no-traffic
     * run separates the takeover burst from continuous line noise. */
    uint32_t rx_bytes_1s, rx_errors_1s;
    bool rx_snap_taken;
    /* C06 session: handshake/ownership/heartbeat lease + action ledger
     * (the ledger is only committed/reset here in M2; admission lands
     * in C07, so last_consumed_seq is the ledger high-water). */
    MbSession session;
    MbLedger ledger;
    MbLink prev_link;
    uint32_t last_challenge_ms;
} BridgeApp;

static uint32_t bridge_now(BridgeApp* app) {
    return furi_get_tick() - app->started_tick;
}

/* Trace pushes happen from thread context only (main loop / init /
 * teardown), never from the serial ISR (C02 rule). */
static void bridge_trace(BridgeApp* app, uint16_t event, uint64_t job_id, uint32_t arg0) {
    mb_diag_push(&app->diag, bridge_now(app), event, job_id, 0, arg0, 0);
}

static const char* bridge_job_state_short(BridgeApp* app) {
    switch(mb_executor_state(&app->exec)) {
    case MB_JOB_ACCEPTED:
        return "ACPT";
    case MB_JOB_STARTING:
        return "STRT";
    case MB_JOB_RUNNING:
        return "RUN";
    case MB_JOB_STOPPING:
        return "STOP";
    case MB_JOB_TERMINAL:
        return "TERM";
    case MB_JOB_FAULTED:
        return "FLT";
    default:
        return "IDLE";
    }
}

static const char* bridge_link_short(MbLink state) {
    switch(state) {
    case MB_LIVE:
        return "LIVE";
    case MB_SUSPECT:
        return "SUSP";
    case MB_EXPIRED:
        return "EXPD";
    default:
        return "WAIT";
    }
}

static void bridge_draw_callback(Canvas* canvas, void* context) {
    BridgeApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Muse Bridge");
    canvas_set_font(canvas, FontSecondary);
    char line[48];
    snprintf(line, sizeof(line), "%s UART %s", BRIDGE_BUILD_ID, app->uart_acquired ? "OWNED" : "waiting");
    canvas_draw_str(canvas, 2, 22, line);
    snprintf(
        line,
        sizeof(line),
        "frames %lu crc_err %lu",
        (unsigned long)app->valid_frames,
        (unsigned long)app->crc_errors);
    canvas_draw_str(canvas, 2, 33, line);
    snprintf(
        line,
        sizeof(line),
        "rx %lu err %lu drop %lu tr %lu",
        (unsigned long)app->rx_bytes,
        (unsigned long)app->rx_errors,
        (unsigned long)app->ring_drops,
        (unsigned long)app->diag.next_seq - 1);
    canvas_draw_str(canvas, 2, 44, line);
    uint32_t elapsed_s = (furi_get_tick() - app->started_tick) / furi_kernel_get_tick_frequency();
    snprintf(
        line,
        sizeof(line),
        "up %lus %s %s f%lu",
        (unsigned long)elapsed_s,
        bridge_job_state_short(app),
        bridge_link_short(app->session.lease.state),
        (unsigned long)mb_fake_execution_count());
    canvas_draw_str(canvas, 2, 55, line);
}

static void bridge_input_callback(InputEvent* event, void* context) {
    BridgeApp* app = context;
    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        /* §9.5: Back while a job runs requests a stop; the main loop
         * exits once the slot is idle. */
        app->back_pressed = true;
    }
}

/* Interrupt context: callback-only byte reads, zero-wait ring writes. */
static void bridge_rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    BridgeApp* app = context;
    if(event == FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t byte = furi_hal_serial_async_rx(handle);
            app->rx_bytes++;
            if(furi_stream_buffer_send(app->rx_stream, &byte, 1, 0) == 0) {
                app->ring_drops++;
                app->ring_overflowed = true;
            }
        }
    } else {
        app->rx_errors++;
    }
}

static void bridge_send_frame_as(
    BridgeApp* app,
    const MbIdentity* id,
    uint8_t type,
    uint16_t op,
    uint16_t status,
    uint64_t correlation,
    const uint8_t* payload,
    size_t payload_len) {
    uint8_t wire[516 + 1];
    size_t n = mb_frame_encode(id, type, op, status, correlation, 0, payload, payload_len, wire, 516);
    if(n == 0) return;
    wire[n] = 0x00;
    furi_hal_serial_tx(app->serial, wire, n + 1);
}

/* Bootstrap replies (PING/GET_INFO) carry our boot ID with zero session
 * and generation: they are diagnostics, valid before any session. */
static void bridge_send_frame(
    BridgeApp* app,
    uint8_t type,
    uint16_t op,
    uint16_t status,
    uint64_t correlation,
    const uint8_t* payload,
    size_t payload_len) {
    MbIdentity id = {0};
    memcpy(id.boot, app->boot_id, 16);
    bridge_send_frame_as(app, &id, type, op, status, correlation, payload, payload_len);
}

static uint8_t bridge_executor_state_byte(BridgeApp* app) {
    MbJobState st = mb_executor_state(&app->exec);
    if(st == MB_JOB_FAULTED) return 2;
    if(st == MB_JOB_IDLE) return 0;
    return 1;
}

static bool bridge_identity_is_zero(const MbIdentity* id) {
    static const uint8_t zeros[16] = {0};
    return memcmp(id->boot, zeros, 16) == 0 && memcmp(id->session, zeros, 16) == 0 &&
           id->generation == 0;
}

/* C06 handshake dispatch. rx_tick is the frame-completion tick taken in
 * the drain loop: the earliest moment the complete proof existed. */
static void bridge_handle_handshake(BridgeApp* app, MbFrame* frame, uint32_t rx_tick) {
    uint32_t now = furi_get_tick();
    if(frame->type == 1) { /* HELLO */
        uint8_t client[16], nonce[16], prior_boot[16], prior_session[16];
        if(!bridge_identity_is_zero(&frame->identity) ||
           !mb_parse_hello(frame->payload, frame->length, client, nonce, prior_boot, prior_session)) {
            bridge_send_frame(app, 2, 0, MB_INVALID_FRAME, frame->correlation, NULL, 0);
            return;
        }
        MbHandshakeResult r = mb_session_on_hello(
            &app->session, &app->ledger, client, nonce, prior_boot, prior_session, now);
        if(r.status != MB_OK) {
            /* Error status carries no proposal (section 6.4). */
            bridge_send_frame(app, 2, 0, r.status, frame->correlation, NULL, 0);
            return;
        }
        uint8_t payload[MB_HELLO_REPLY_LEN];
        size_t n = mb_build_hello_reply(
            payload, sizeof(payload), &app->session, &r, 0, bridge_executor_state_byte(app));
        bridge_send_frame_as(app, &r.identity, 2, 0, MB_OK, frame->correlation, payload, n);
        return;
    }
    if(frame->type == 3) { /* CONFIRM */
        uint8_t client[16], nonce[16];
        uint64_t challenge = 0;
        if(!mb_parse_confirm(frame->payload, frame->length, client, nonce, &challenge)) {
            bridge_send_frame(app, 9, 3, MB_INVALID_FRAME, frame->correlation, NULL, 0);
            return;
        }
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        bool cleanup_done = !mb_executor_busy(&app->exec);
        furi_mutex_release(app->exec_mutex);
        /* An exact duplicate CONFIRM is served from the session cache;
         * only the fresh commit bumps the diagnostic session epoch. */
        bool cached = app->session.done_valid &&
                      memcmp(app->session.done_client, client, 16) == 0 &&
                      memcmp(app->session.done_nonce, nonce, 16) == 0;
        MbHandshakeResult r = mb_session_on_confirm(
            &app->session, &app->ledger, client, nonce, challenge, &frame->identity,
            rx_tick, now, cleanup_done);
        if(r.status != MB_OK) {
            bridge_send_frame(app, 9, 3, r.status, frame->correlation, NULL, 0);
            return;
        }
        if(!cached) mb_diag_new_session(&app->diag);
        uint8_t payload[MB_READY_LEN];
        size_t n = mb_build_ready(
            payload, sizeof(payload), &app->session, nonce, r.last_consumed_seq, 0,
            bridge_executor_state_byte(app));
        bridge_send_frame_as(app, &r.identity, 4, 0, MB_OK, frame->correlation, payload, n);
        return;
    }
    if(frame->type == 6) { /* HEARTBEAT_ECHO: no reply, it IS the reply */
        if(frame->length == MB_COUNTER_LEN) {
            mb_session_echo(
                &app->session, &frame->identity, mb_u64(frame->payload), rx_tick, now);
        }
        return;
    }
    app->unsupported++;
    bridge_send_frame(app, 9, frame->op, MB_UNSUPPORTED, frame->correlation, NULL, 0);
}

#define BRIDGE_OP_FAKE_RUN 0x7F01

static void bridge_send_event(BridgeApp* app, const MbDataRecord* rec) {
    uint8_t wire[516 + 1];
    size_t n = mb_frame_encode(
        &app->session.lease.identity, 11, app->active_op, MB_OK,
        app->active_seq, rec->seq, rec->bytes, rec->len, wire, 516);
    if(n == 0) return;
    wire[n] = 0x00;
    furi_hal_serial_tx(app->serial, wire, n + 1);
}

/* ---- C10 GPIO hardware glue ----
 * Header pin -> MCU pin per plan S13. Pins outside the P1-A allowlist
 * (2..7) never reach here: validation rejects them first. Configure
 * follows the R05 order — preload the level, then init the mode — so
 * a fresh output does not glitch. Restore is the documented P1-A
 * default: analog, no pull. */
static const GpioPin* bridge_gpio_furi_pin(uint8_t header_pin) {
    switch(header_pin) {
    case 2:
        return &gpio_ext_pa7;
    case 3:
        return &gpio_ext_pa6;
    case 4:
        return &gpio_ext_pa4;
    case 5:
        return &gpio_ext_pb3;
    case 6:
        return &gpio_ext_pb2;
    case 7:
        return &gpio_ext_pc3;
    default:
        return NULL;
    }
}

static void bridge_gpio_hal_config(void* ctx, uint8_t pin, uint8_t mode, uint8_t pull, uint8_t initial) {
    UNUSED(ctx);
    const GpioPin* p = bridge_gpio_furi_pin(pin);
    if(p == NULL) return;
    furi_hal_gpio_write(p, initial != 0); /* R05 preload before mode */
    if(mode == MB_GPIO_MODE_OUTPUT) {
        furi_hal_gpio_init(p, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    } else {
        GpioPull fp = pull == MB_GPIO_PULL_UP     ? GpioPullUp :
                      pull == MB_GPIO_PULL_DOWN   ? GpioPullDown :
                                                    GpioPullNo;
        furi_hal_gpio_init(p, GpioModeInput, fp, GpioSpeedVeryHigh);
    }
}

static void bridge_gpio_hal_write(void* ctx, uint8_t pin, uint8_t value) {
    UNUSED(ctx);
    const GpioPin* p = bridge_gpio_furi_pin(pin);
    if(p != NULL) furi_hal_gpio_write(p, value != 0);
}

static bool bridge_gpio_hal_read(void* ctx, uint8_t pin) {
    UNUSED(ctx);
    const GpioPin* p = bridge_gpio_furi_pin(pin);
    return p != NULL && furi_hal_gpio_read(p);
}

static void bridge_gpio_hal_restore(void* ctx, uint8_t pin) {
    UNUSED(ctx);
    const GpioPin* p = bridge_gpio_furi_pin(pin);
    if(p != NULL) furi_hal_gpio_init(p, GpioModeAnalog, GpioPullNo, GpioSpeedVeryHigh);
}

/* ---- C11 ADC glue (furi_hal_adc, default 0-2048 mV scale) ---- */
static bool bridge_adc_prepare_pin(void* ctx, uint8_t pin) {
    UNUSED(ctx);
    const GpioPin* p = bridge_gpio_furi_pin(pin);
    if(p == NULL) return false;
    furi_hal_gpio_init(p, GpioModeAnalog, GpioPullNo, GpioSpeedVeryHigh);
    return true;
}

static void bridge_adc_restore_pin(void* ctx, uint8_t pin) {
    bridge_adc_prepare_pin(ctx, pin); /* unclaimed pins end analog */
}

static bool bridge_adc_read_sample(void* ctx, uint8_t channel, uint16_t* raw, uint16_t* mv) {
    UNUSED(ctx);
    FuriHalAdcHandle* h = furi_hal_adc_acquire();
    if(h == NULL) return false;
    furi_hal_adc_configure(h); /* Scale2048, oversample 64 */
    *raw = furi_hal_adc_read(h, (FuriHalAdcChannel)channel);
    *mv = (uint16_t)furi_hal_adc_convert_to_voltage(h, *raw);
    furi_hal_adc_release(h);
    return true;
}

/* ---- C11 notification glue. Static sequence storage (retained for
 * the service), every effect at most ~100 ms, well under the 250 ms
 * bound. All calls run on the executor thread via the module. ---- */
static const NotificationSequence bridge_seq_green_flash = {
    &message_green_255,
    &message_delay_100,
    &message_green_0,
    NULL};
static const NotificationSequence bridge_seq_short_beep = {
    &message_note_a5,
    &message_delay_100,
    &message_sound_off,
    NULL};
static const NotificationSequence bridge_seq_short_vibro = {
    &message_vibro_on,
    &message_delay_100,
    &message_vibro_off,
    NULL};
/* Bench signals: double beep = a listening period starts/ends;
 * a one-second blue blink repeats while the device is listening. */
static const NotificationSequence bridge_seq_double_beep = {
    &message_note_a5,
    &message_delay_100,
    &message_sound_off,
    &message_delay_50,
    &message_note_a5,
    &message_delay_100,
    &message_sound_off,
    NULL};
static const NotificationSequence bridge_seq_listen_blink = {
    &message_blue_255,
    &message_delay_100,
    &message_blue_0,
    NULL};

static void bridge_notify_play(void* ctx, uint8_t effect) {
    BridgeApp* app = ctx;
    if(app == NULL || app->notification == NULL) return;
    const NotificationSequence* seq = NULL;
    switch(effect) {
    case MB_NOTIFY_GREEN_FLASH:
        seq = &bridge_seq_green_flash;
        break;
    case MB_NOTIFY_SHORT_BEEP:
        seq = &bridge_seq_short_beep;
        break;
    case MB_NOTIFY_SHORT_VIBRATION:
        seq = &bridge_seq_short_vibro;
        break;
    case MB_NOTIFY_DOUBLE_BEEP:
        seq = &bridge_seq_double_beep; /* explicit: always plays */
        break;
    case MB_NOTIFY_LISTEN_ON:
        app->signal_listen = true;
        return;
    case MB_NOTIFY_LISTEN_OFF:
        app->signal_listen = false;
        return;
    case MB_NOTIFY_SIGNALS_OFF:
        app->signal_muted = true;
        return;
    case MB_NOTIFY_SIGNALS_ON:
        app->signal_muted = false;
        return;
    default:
        return;
    }
    notification_message_block(app->notification, seq);
}

/* Automatic listening signals for hardware jobs (respect the mute
 * gate; called on the executor thread from the module glue). */
static void bridge_signal_beeps(BridgeApp* app) {
    if(app == NULL || app->notification == NULL || app->signal_muted) return;
    notification_message_block(app->notification, &bridge_seq_double_beep);
}

static void bridge_signal_set_listen(BridgeApp* app, bool on) {
    if(app != NULL) app->signal_listen = on;
}

/* ---- C12 IR glue (InfraredWorker). The received-signal callback
 * runs on the worker thread: copy the decoded values into the
 * module ring immediately, never retain the message pointer. ---- */
static void bridge_ir_on_signal(void* ctx, InfraredWorkerSignal* sig) {
    UNUSED(ctx);
    if(sig == NULL) return;
    if(!infrared_worker_signal_is_decoded(sig)) {
        /* C15 raw path: copy the worker's timings immediately. */
        const uint32_t* timings = NULL;
        size_t count = 0;
        infrared_worker_get_raw_signal(sig, &timings, &count);
        if(timings != NULL && count > 0) mb_ir_raw_on_timings(timings, count);
        return;
    }
    const InfraredMessage* m = infrared_worker_get_decoded_signal(sig);
    if(m == NULL || m->protocol != InfraredProtocolNEC) return;
    mb_ir_on_decoded(MB_IR_PROTOCOL_NEC, m->address, m->command, m->repeat);
}

static bool bridge_ir_rx_start(void* ctx) {
    BridgeApp* app = ctx;
    if(app->ir_worker != NULL) return false;
    app->ir_worker = infrared_worker_alloc();
    if(app->ir_worker == NULL) return false;
    infrared_worker_rx_set_received_signal_callback(
        app->ir_worker, bridge_ir_on_signal, app);
    infrared_worker_rx_enable_signal_decoding(app->ir_worker, true);
    bridge_signal_beeps(app); /* listening opens when the beeps end */
    infrared_worker_rx_start(app->ir_worker);
    bridge_signal_set_listen(app, true);
    return true;
}

static bool bridge_ir_rx_start_raw(void* ctx) {
    BridgeApp* app = ctx;
    if(app->ir_worker != NULL) return false;
    app->ir_worker = infrared_worker_alloc();
    if(app->ir_worker == NULL) return false;
    infrared_worker_rx_set_received_signal_callback(
        app->ir_worker, bridge_ir_on_signal, app);
    infrared_worker_rx_enable_signal_decoding(app->ir_worker, false);
    bridge_signal_beeps(app); /* listening opens when the beeps end */
    infrared_worker_rx_start(app->ir_worker);
    bridge_signal_set_listen(app, true);
    return true;
}

static void bridge_ir_rx_stop(void* ctx) {
    BridgeApp* app = ctx;
    bridge_signal_set_listen(app, false);
    if(app->ir_worker == NULL) return;
    infrared_worker_rx_stop(app->ir_worker);
    infrared_worker_free(app->ir_worker);
    app->ir_worker = NULL;
    bridge_signal_beeps(app); /* listening has ended */
}

/* ---- C13 IR TX glue. The get-signal callback runs on the worker
 * thread: the portable finite provider decides New/Stop; on New we
 * stage the decoded message into the worker and return New. The
 * message-sent callback just bumps the portable counter. ---- */
static InfraredWorkerGetSignalResponse bridge_ir_tx_get_signal(void* ctx, InfraredWorker* instance) {
    UNUSED(ctx);
    /* C15 raw train first: its provider answers New exactly once for
     * a staged object and Stop otherwise (including when no raw job
     * is active), so the decoded provider is consulted only on Stop. */
    const uint32_t* raw_timings = NULL;
    uint16_t raw_count = 0;
    if(mb_ir_tx_raw_supply(&raw_timings, &raw_count) == MB_IR_TX_NEW) {
        infrared_worker_set_raw_signal(
            instance, raw_timings, raw_count,
            MB_OBJ_REPLAY_CARRIER_HZ, (float)MB_OBJ_REPLAY_DUTY_PERMILLE / 1000.0f);
        return InfraredWorkerGetSignalResponseNew;
    }
    uint8_t proto = 0;
    uint32_t addr = 0, cmd = 0;
    if(mb_ir_tx_supply(&proto, &addr, &cmd) != MB_IR_TX_NEW) {
        return InfraredWorkerGetSignalResponseStop;
    }
    InfraredMessage msg = {
        .protocol = InfraredProtocolNEC, .address = addr, .command = cmd, .repeat = false};
    infrared_worker_set_decoded_signal(instance, &msg);
    return InfraredWorkerGetSignalResponseNew;
}

static void bridge_ir_tx_sent(void* ctx) {
    UNUSED(ctx);
    mb_ir_tx_on_sent();
    mb_ir_tx_raw_on_sent();
}

static bool bridge_ir_tx_start(void* ctx) {
    BridgeApp* app = ctx;
    if(app->ir_worker != NULL) return false;
    app->ir_worker = infrared_worker_alloc();
    if(app->ir_worker == NULL) return false;
    infrared_worker_tx_set_get_signal_callback(app->ir_worker, bridge_ir_tx_get_signal, app);
    infrared_worker_tx_set_signal_sent_callback(app->ir_worker, bridge_ir_tx_sent, app);
    infrared_worker_tx_start(app->ir_worker);
    return true;
}

static void bridge_ir_tx_stop(void* ctx) {
    BridgeApp* app = ctx;
    if(app->ir_worker == NULL) return;
    infrared_worker_tx_stop(app->ir_worker); /* waits out in-flight signal */
    infrared_worker_free(app->ir_worker);
    app->ir_worker = NULL;
}

/* Semantic validation of a GPIO REQUEST (claim state read under the
 * executor mutex). On success, `out` carries the job params with the
 * output hold folded into an absolute deadline tick. */
static MbStatus bridge_gpio_validate(BridgeApp* app, MbFrame* frame, MbGpioParams* out) {
    uint32_t hz = furi_kernel_get_tick_frequency();
    *out = (MbGpioParams){0};
    out->op = frame->op;
    MbStatus v = MB_OK;
    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    switch(frame->op) {
    case MB_GPIO_OP_CONFIG: {
        if(frame->length != 8) {
            v = MB_INVALID_ARGUMENT;
            break;
        }
        uint8_t pin = frame->payload[0];
        uint8_t mode = frame->payload[1];
        uint8_t pull = frame->payload[2];
        uint8_t initial = frame->payload[3];
        uint32_t hold_ms = mb_u32(frame->payload + 4);
        v = mb_gpio_validate_config(&app->gpio_state, pin, mode, pull, initial, hold_ms);
        if(v == MB_OK) {
            out->pin = pin;
            out->mode = mode;
            out->pull = pull;
            out->value = initial;
            if(mode == MB_GPIO_MODE_OUTPUT) {
                uint32_t hold_ticks = 0;
                mb_ticks_from_ms(hold_ms ? hold_ms : MB_GPIO_DEFAULT_HOLD_MS, hz, &hold_ticks);
                out->deadline_tick = furi_get_tick() + hold_ticks;
            }
        }
        break;
    }
    case MB_GPIO_OP_READ:
        if(frame->length != 1) {
            v = MB_INVALID_ARGUMENT;
            break;
        }
        out->pin = frame->payload[0];
        v = mb_gpio_validate_read(&app->gpio_state, out->pin);
        break;
    case MB_GPIO_OP_WRITE:
        if(frame->length != 2) {
            v = MB_INVALID_ARGUMENT;
            break;
        }
        out->pin = frame->payload[0];
        out->value = frame->payload[1];
        v = mb_gpio_validate_write(&app->gpio_state, out->pin, out->value);
        break;
    case MB_GPIO_OP_RELEASE:
        if(frame->length != 1) {
            v = MB_INVALID_ARGUMENT;
            break;
        }
        out->pin = frame->payload[0];
        v = mb_gpio_validate_release(&app->gpio_state, out->pin);
        break;
    default:
        v = MB_UNSUPPORTED;
        break;
    }
    furi_mutex_release(app->exec_mutex);
    return v;
}

/* Semantic validation for ADC_READ / NOTIFY / IR_RX_START requests. */
static MbStatus bridge_ir_validate_req(MbFrame* frame, MbIrParams* out) {
    *out = (MbIrParams){0};
    if(frame->length != 6) return MB_INVALID_ARGUMENT;
    out->protocol_filter = mb_u16(frame->payload);
    out->timeout_ms = mb_u32(frame->payload + 2);
    MbStatus v;
    return mb_ir_validate_params(out, &v) ? MB_OK : v;
}

static MbStatus bridge_ir_tx_validate_req(MbFrame* frame, MbIrTxParams* out) {
    *out = (MbIrTxParams){0};
    if(frame->length != 15) return MB_INVALID_ARGUMENT;
    out->protocol = mb_u16(frame->payload);
    out->address = mb_u32(frame->payload + 2);
    out->command = mb_u32(frame->payload + 6);
    out->frame_count = frame->payload[10];
    out->timeout_ms = mb_u32(frame->payload + 11);
    MbStatus v;
    return mb_ir_tx_validate_params(out, &v) ? MB_OK : v;
}

/* ---- C15 validators ---- */
static uint32_t bridge_now_ms(void) {
    return furi_get_tick(); /* tick base is 1 kHz on this target */
}

static bool bridge_obj_pin(void* ctx, uint64_t object_id) {
    BridgeApp* app = ctx;
    return mb_object_pin(&app->objects, object_id) == MB_OK;
}

static void bridge_obj_unpin(void* ctx, uint64_t object_id) {
    BridgeApp* app = ctx;
    mb_object_unpin(&app->objects, object_id);
}

static MbStatus bridge_ir_raw_rx_validate_req(MbFrame* frame, MbIrRawParams* out) {
    *out = (MbIrRawParams){0};
    if(frame->length != 4) return MB_INVALID_ARGUMENT;
    out->timeout_ms = mb_u32(frame->payload);
    MbStatus v;
    return mb_ir_raw_validate_params(out, &v) ? MB_OK : v;
}

static MbStatus bridge_ir_tx_raw_validate_req(BridgeApp* app, MbFrame* frame, MbIrTxRawParams* out) {
    *out = (MbIrTxRawParams){0};
    if(frame->length != 13) return MB_INVALID_ARGUMENT;
    out->object_id = mb_u64(frame->payload);
    out->frame_count = frame->payload[8];
    out->timeout_ms = mb_u32(frame->payload + 9);
    MbStatus v;
    if(!mb_ir_tx_raw_validate_params(out, &v)) return v;
    /* Store gate: the object must exist, belong to this session, and
     * be complete; its timings are staged for the job (consumed by
     * the module's start). Serialized with executor state. */
    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    const uint32_t* timings = NULL;
    uint16_t count = 0;
    MbStatus st = mb_object_tx_check(
        &app->objects, &app->session.lease.identity, out->object_id, &timings, &count);
    if(st == MB_OK) mb_ir_tx_raw_stage(timings, count);
    furi_mutex_release(app->exec_mutex);
    return st;
}

static MbStatus bridge_object_validate_req(MbFrame* frame, MbObjParams* out) {
    *out = (MbObjParams){0};
    out->op = frame->op;
    switch(frame->op) {
    case MB_OBJ_OP_BEGIN: {
        if(frame->length != 14) return MB_INVALID_ARGUMENT;
        out->object_type = frame->payload[0];
        out->timing_count = mb_u16(frame->payload + 1);
        out->carrier_hz = mb_u32(frame->payload + 3);
        out->duty_permille = mb_u16(frame->payload + 7);
        out->starts_with_mark = frame->payload[9] != 0;
        out->checksum = mb_u32(frame->payload + 10);
        /* Pure field rules mirror module_object_begin's, so a bad
         * shape is a consumed rejection, not an admitted failure;
         * state-dependent refusals (BUSY) stay execution outcomes. */
        if(out->object_type != MB_OBJ_TYPE_RAW_IR) return MB_INVALID_ARGUMENT;
        if(out->timing_count == 0 || out->timing_count > MB_OBJ_MAX_TIMINGS)
            return MB_INVALID_ARGUMENT;
        if(out->carrier_hz != MB_OBJ_REPLAY_CARRIER_HZ) return MB_INVALID_ARGUMENT;
        if(out->duty_permille != MB_OBJ_REPLAY_DUTY_PERMILLE) return MB_INVALID_ARGUMENT;
        if(frame->payload[9] != 1) return MB_INVALID_ARGUMENT;
        return MB_OK;
    }
    case MB_OBJ_OP_CHUNK: {
        if(frame->length < 11) return MB_INVALID_ARGUMENT;
        out->object_id = mb_u64(frame->payload);
        out->offset = mb_u16(frame->payload + 8);
        out->count = frame->payload[10];
        if(out->count == 0 || out->count > MB_OBJ_CHUNK_MAX) return MB_INVALID_ARGUMENT;
        if(frame->length != (uint16_t)(11 + 4 * out->count)) return MB_INVALID_ARGUMENT;
        for(uint8_t i = 0; i < out->count; i++)
            out->durations[i] = mb_u32(frame->payload + 11 + 4 * i);
        return MB_OK;
    }
    case MB_OBJ_OP_COMMIT:
    case MB_OBJ_OP_RELEASE: {
        if(frame->length != 8) return MB_INVALID_ARGUMENT;
        out->object_id = mb_u64(frame->payload);
        return MB_OK;
    }
    default:
        return MB_INVALID_ARGUMENT;
    }
}

static MbStatus bridge_adc_validate_req(BridgeApp* app, MbFrame* frame, MbAdcParams* out) {
    *out = (MbAdcParams){0};
    if(frame->length != 2) return MB_INVALID_ARGUMENT;
    uint8_t pin = frame->payload[0];
    uint8_t samples = frame->payload[1];
    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    bool claimed = pin < 32 && app->gpio_state.pins[pin].claimed;
    furi_mutex_release(app->exec_mutex);
    MbStatus v = mb_adc_validate(pin, samples, claimed);
    if(v == MB_OK) {
        out->pin = pin;
        out->samples = samples;
    }
    return v;
}

static MbStatus bridge_notify_validate_req(MbFrame* frame, MbNotifyParams* out) {
    *out = (MbNotifyParams){0};
    if(frame->length != 1) return MB_INVALID_ARGUMENT;
    MbStatus v = mb_notify_validate(frame->payload[0]);
    if(v == MB_OK) out->effect = frame->payload[0];
    return v;
}

/* Retained result bytes for a finished GPIO job (status first, then
 * op-specific fields; documented in docs/protocol.md). */
static size_t bridge_gpio_result(BridgeApp* app, uint16_t op, MbStatus status, uint8_t* out) {
    size_t w = 0;
    mb_put_u16(out + w, (uint16_t)status);
    w += 2;
    const MbGpioClaim* c = &app->gpio_state.pins[app->gpio_params.pin];
    switch(op) {
    case MB_GPIO_OP_CONFIG:
        out[w++] = app->gpio_params.pin;
        out[w++] = app->gpio_params.mode;
        out[w++] = app->gpio_params.pull;
        break;
    case MB_GPIO_OP_READ:
        out[w++] = app->gpio_params.pin;
        out[w++] = c->claimed ? c->mode : 0xFF;
        out[w++] = c->claimed ? c->pull : 0xFF;
        out[w++] = mb_gpio_module_last_level();
        break;
    case MB_GPIO_OP_WRITE:
        out[w++] = app->gpio_params.pin;
        out[w++] = app->gpio_params.value;
        break;
    case MB_GPIO_OP_RELEASE:
        out[w++] = app->gpio_params.pin;
        break;
    case MB_ADC_OP_READ:
        out[w++] = app->adc_params.pin;
        out[w++] = app->adc_state.channel;
        out[w++] = app->adc_params.samples;
        mb_put_u16(out + w, app->adc_state.raw_mean);
        w += 2;
        mb_put_u16(out + w, app->adc_state.mv_mean);
        w += 2;
        break;
    case MB_NOTIFY_OP:
        out[w++] = app->notify_params.effect;
        break;
    case MB_IR_OP_RX_START: {
        MbIrSummary sum;
        mb_ir_last_summary(&sum);
        mb_put_u32(out + w, sum.decoded);
        w += 4;
        mb_put_u32(out + w, sum.emitted);
        w += 4;
        mb_put_u32(out + w, sum.dropped);
        w += 4;
        break;
    }
    case MB_IR_OP_TX_DECODED: {
        MbIrTxSummary sum;
        mb_ir_tx_last_summary(&sum);
        mb_put_u32(out + w, sum.supplied);
        w += 4;
        mb_put_u32(out + w, sum.sent);
        w += 4;
        break;
    }
    case MB_OBJ_OP_RX_RAW_START: {
        /* Publish the finished capture into the object store; the
         * object id is this action's own sequence. Only natural
         * completions publish: a cancelled/stopped capture commits
         * nothing. */
        mb_put_u64(out + w, 0);
        w += 8; /* object_id, filled below when published */
        MbIrRawSummary rs;
        mb_ir_raw_last_summary(&rs);
        uint16_t count = 0;
        uint32_t crc = 0;
        uint8_t complete = 0;
        if(rs.have && (status == MB_OK || status == MB_OVERFLOW)) {
            count = rs.count > 0xFFFF ? 0xFFFF : (uint16_t)rs.count;
            MbStatus pub = mb_object_publish_capture(
                &app->objects, &app->session.lease.identity, app->active_seq,
                rs.timings, rs.count, furi_get_tick());
            if(pub == MB_OK) {
                MbObjectReadResult rr;
                if(mb_object_read(
                       &app->objects, &app->session.lease.identity, app->active_seq,
                       0, 1, &rr) == MB_OK) {
                    crc = rr.crc;
                }
                complete = 1;
                mb_put_u64(out + 2, app->active_seq);
            }
        }
        mb_put_u16(out + w, count);
        w += 2;
        mb_put_u32(out + w, crc);
        w += 4;
        out[w++] = complete;
        break;
    }
    case MB_OBJ_OP_TX_RAW: {
        MbIrTxRawSummary sum;
        mb_ir_tx_raw_last_summary(&sum);
        mb_put_u32(out + w, sum.supplied);
        w += 4;
        mb_put_u32(out + w, sum.sent);
        w += 4;
        break;
    }
    case MB_OBJ_OP_BEGIN:
    case MB_OBJ_OP_CHUNK:
    case MB_OBJ_OP_COMMIT:
    case MB_OBJ_OP_RELEASE: {
        MbObjectOpResult r;
        mb_object_last_result(&r);
        mb_put_u64(out + w, r.object_id);
        w += 8;
        mb_put_u32(out + w, r.crc);
        w += 4;
        break;
    }
    default:
        break;
    }
    return w;
}

/* C07 action admission. Sequence = frame correlation; canonical payload
 * is compared byte-exact by the ledger. Replies: RESULT for refusals and
 * consumed rejections, ACCEPTED/COMPLETE (replayed verbatim for exact
 * duplicates) for owned actions. */
static void bridge_handle_request(BridgeApp* app, MbFrame* frame) {
    /* Pure semantic validation first: no executor state is touched. */
    MbStatus rejection = MB_OK;
    uint32_t duration_ms = 0;
    uint16_t interval_ms = 0;
    MbGpioParams gpio = {0};
    MbAdcParams adc = {0};
    MbNotifyParams notify = {0};
    MbIrParams ir = {0};
    MbIrTxParams irtx = {0};
    MbIrRawParams rawrx = {0};
    MbIrTxRawParams rawtx = {0};
    MbObjParams obj = {0};
    bool is_gpio = frame->op >= MB_GPIO_OP_CONFIG && frame->op <= MB_GPIO_OP_RELEASE;
    bool is_adc = frame->op == MB_ADC_OP_READ;
    bool is_notify = frame->op == MB_NOTIFY_OP;
    bool is_ir = frame->op == MB_IR_OP_RX_START;
    bool is_ir_tx = frame->op == MB_IR_OP_TX_DECODED;
    bool is_raw_rx = frame->op == MB_OBJ_OP_RX_RAW_START;
    bool is_raw_tx = frame->op == MB_OBJ_OP_TX_RAW;
    bool is_obj = frame->op == MB_OBJ_OP_BEGIN || frame->op == MB_OBJ_OP_CHUNK ||
                  frame->op == MB_OBJ_OP_COMMIT || frame->op == MB_OBJ_OP_RELEASE;
#if !MB_RELEASE_BUILD
    if(frame->op == BRIDGE_OP_FAKE_RUN) {
        if(frame->length != 6) {
            rejection = MB_INVALID_ARGUMENT;
        } else {
            duration_ms = mb_u32(frame->payload);
            interval_ms = mb_u16(frame->payload + 4);
            if(duration_ms > 60000) {
                rejection = MB_INVALID_ARGUMENT;
            } else if(duration_ms == 0) {
                duration_ms = 10000; /* documented default (plan 6.5) */
            }
        }
    } else
#endif
        if(is_gpio) {
        rejection = bridge_gpio_validate(app, frame, &gpio);
    } else if(is_adc) {
        rejection = bridge_adc_validate_req(app, frame, &adc);
    } else if(is_notify) {
        rejection = bridge_notify_validate_req(frame, &notify);
    } else if(is_ir) {
        rejection = bridge_ir_validate_req(frame, &ir);
    } else if(is_ir_tx) {
        rejection = bridge_ir_tx_validate_req(frame, &irtx);
    } else if(is_raw_rx) {
        rejection = bridge_ir_raw_rx_validate_req(frame, &rawrx);
    } else if(is_raw_tx) {
        rejection = bridge_ir_tx_raw_validate_req(app, frame, &rawtx);
    } else if(is_obj) {
        rejection = bridge_object_validate_req(frame, &obj);
    } else {
        rejection = MB_UNSUPPORTED;
    }
    if(rejection == MB_OK) {
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        bool busy = mb_executor_busy(&app->exec);
        furi_mutex_release(app->exec_mutex);
        if(busy) rejection = MB_BUSY;
    }

    MbAdmission adm = mb_action_admit(
        &app->session.lease, &app->ledger, frame, furi_get_tick(), true, rejection);
    if(adm.kind == MB_REFUSED) {
        bridge_trace(app, MB_EV_ACTION_REJECTED, frame->correlation, (uint32_t)adm.status);
        /* Refusals echo the requester's own envelope; nothing is owned. */
        bridge_send_frame_as(
            app, &frame->identity, 9, frame->op, adm.status, frame->correlation, NULL, 0);
        return;
    }
    if(adm.kind == MB_REPLAY) {
        const MbEntry* e = &app->ledger.entry[adm.index];
        bridge_trace(app, MB_EV_ACTION_REPLAYED, frame->correlation, (uint32_t)e->status);
        if(e->state == MB_TERMINAL && e->result_len > 0) {
            /* A finished job: replay its COMPLETE with retained summary. */
            bridge_send_frame_as(
                app, &app->session.lease.identity, 12, frame->op, e->status,
                frame->correlation, e->result, e->result_len);
        } else if(e->state == MB_TERMINAL) {
            /* A consumed immediate rejection: replay its RESULT. */
            bridge_send_frame_as(
                app, &app->session.lease.identity, 9, frame->op, e->status,
                frame->correlation, NULL, 0);
        } else {
            bridge_send_frame_as(
                app, &app->session.lease.identity, 10, frame->op, MB_OK,
                frame->correlation, NULL, 0);
        }
        return;
    }
    /* MB_ADMITTED */
    if(rejection != MB_OK) {
        /* Consumed rejection: terminal in the ledger before any effect. */
        bridge_trace(app, MB_EV_ACTION_REJECTED, frame->correlation, (uint32_t)rejection);
        bridge_send_frame_as(
            app, &app->session.lease.identity, 9, frame->op, rejection,
            frame->correlation, NULL, 0);
        return;
    }

    uint32_t hz = furi_kernel_get_tick_frequency();
    const MbModule* module = &mb_module_fake;
    const void* params = &app->fake_params;
    uint32_t deadline = 0;
    if(is_gpio) {
        /* Instantaneous ops: the whole action completes in the
         * module's start(); the deadline is only a wedge guard. */
        uint32_t grace = 0;
        mb_ticks_from_ms(2000, hz, &grace);
        deadline = grace;
        app->gpio_params = gpio;
        module = &mb_module_gpio;
        params = &app->gpio_params;
    } else if(is_adc || is_notify) {
        /* ADC: a few ms of sampling. NOTIFY: bounded 250 ms effect.
         * Both complete in start(); same wedge guard. */
        uint32_t grace = 0;
        mb_ticks_from_ms(2000, hz, &grace);
        deadline = grace;
        if(is_adc) {
            app->adc_params = adc;
            module = &mb_module_adc;
            params = &app->adc_params;
        } else {
            app->notify_params = notify;
            module = &mb_module_notify;
            params = &app->notify_params;
        }
    } else if(is_ir) {
        /* Timed receive; the module completes itself at timeout,
         * the executor deadline is the 5 s wedge guard behind it. */
        uint32_t dur = 0, grace = 0;
        mb_ticks_from_ms(ir.timeout_ms, hz, &dur);
        mb_ticks_from_ms(5000, hz, &grace);
        deadline = dur + grace;
        app->ir_params = ir;
        module = &mb_module_ir;
        params = &app->ir_params;
    } else if(is_ir_tx) {
        /* Finite transmission; same deadline shape as IR RX: the
         * module's own timeout, plus the 5 s wedge guard. */
        uint32_t dur = 0, grace = 0;
        mb_ticks_from_ms(irtx.timeout_ms, hz, &dur);
        mb_ticks_from_ms(5000, hz, &grace);
        deadline = dur + grace;
        app->ir_tx_params = irtx;
        module = &mb_module_ir_tx;
        params = &app->ir_tx_params;
    } else if(is_raw_rx) {
        /* Timed raw capture; same deadline shape as decoded RX. */
        uint32_t dur = 0, grace = 0;
        mb_ticks_from_ms(rawrx.timeout_ms, hz, &dur);
        mb_ticks_from_ms(5000, hz, &grace);
        deadline = dur + grace;
        app->raw_rx_params = rawrx;
        module = &mb_module_ir_raw_rx;
        params = &app->raw_rx_params;
    } else if(is_raw_tx) {
        /* Finite raw transmission; same deadline shape. */
        uint32_t dur = 0, grace = 0;
        mb_ticks_from_ms(rawtx.timeout_ms, hz, &dur);
        mb_ticks_from_ms(5000, hz, &grace);
        deadline = dur + grace;
        app->raw_tx_params = rawtx;
        module = &mb_module_ir_tx_raw;
        params = &app->raw_tx_params;
    } else if(is_obj) {
        /* Instantaneous store action: completes in start(); the
         * deadline is only the wedge guard. */
        uint32_t grace = 0;
        mb_ticks_from_ms(2000, hz, &grace);
        deadline = grace;
        app->obj_params = obj;
        module = &mb_module_object;
        params = &app->obj_params;
    } else {
        uint32_t dur_ticks = 0, int_ticks = 0, grace = 0;
        mb_ticks_from_ms(duration_ms, hz, &dur_ticks);
        if(interval_ms) mb_ticks_from_ms(interval_ms, hz, &int_ticks);
        mb_ticks_from_ms(5000, hz, &grace);
        deadline = dur_ticks + grace;
        app->fake_params = (MbFakeParams){
            .duration_ticks = dur_ticks,
            .emit_interval_ticks = int_ticks,
            .emit_size = 8,
            .fail_worker = false,
            .fail_cleanup = false};
    }
    app->active_seq = frame->correlation;
    app->active_op = frame->op;
    app->admit_generated = app->exec.generated;
    app->admit_dropped = app->exec.dropped;

    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    MbStatus started = mb_executor_admit(
        &app->exec, frame->correlation, module, params, furi_get_tick(), deadline);
    furi_mutex_release(app->exec_mutex);
    if(started != MB_OK) {
        /* Pre-check said free; if the slot raced, finish the ledger
         * record with the truth and do not pretend ownership. */
        mb_ledger_finish(&app->ledger, frame->correlation, started, NULL, 0);
        app->active_seq = 0;
        app->active_op = 0;
        bridge_send_frame_as(
            app, &app->session.lease.identity, 9, frame->op, started,
            frame->correlation, NULL, 0);
        return;
    }
    mb_ledger_mark_started(&app->ledger, frame->correlation);
    bridge_trace(app, MB_EV_ACTION_ADMITTED, frame->correlation, 0);
    bridge_trace(app, MB_EV_JOB_STARTED, frame->correlation, 0);
    bridge_send_frame_as(
        app, &app->session.lease.identity, 10, frame->op, MB_OK,
        frame->correlation, NULL, 0);
}

static void bridge_handle_frame(BridgeApp* app, const uint8_t* encoded, size_t encoded_len, uint32_t rx_tick) {
    MbFrame frame;
    MbCodec rc = mb_frame_decode(encoded, encoded_len, app->decoded, &frame);
    if(rc != MB_CODEC_OK) {
        bridge_trace(app, MB_EV_FRAME_DROP, 0, rc == MB_CODEC_CRC ? 1 : 2);
        if(rc == MB_CODEC_CRC) {
            app->crc_errors++;
        } else {
            app->codec_errors++;
        }
        return;
    }
    app->valid_frames++;

    if(frame.type == 1 || frame.type == 3 || frame.type == 5 || frame.type == 6) {
        bridge_handle_handshake(app, &frame, rx_tick);
        return;
    }
    if(frame.type == 8) { /* REQUEST: C07 actions */
        bridge_handle_request(app, &frame);
        return;
    }
    if(frame.type != 7) {
        app->unsupported++;
        bridge_send_frame(app, 9, frame.op, MB_UNSUPPORTED, frame.correlation, NULL, 0);
        return;
    }

    if(frame.op == 0x0001) { /* PING */
        if(frame.length < 1 || frame.payload[0] != frame.length - 1 || frame.payload[0] > 32) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        uint8_t resp[1 + 32 + 4 + 4];
        resp[0] = frame.payload[0];
        memcpy(resp + 1, frame.payload + 1, frame.payload[0]);
        mb_put_u32(resp + 1 + frame.payload[0], furi_get_tick() - app->started_tick);
        mb_put_u32(resp + 5 + frame.payload[0], furi_kernel_get_tick_frequency());
        bridge_send_frame(
            app, 9, frame.op, MB_OK, frame.correlation, resp, 1u + frame.payload[0] + 8u);
        return;
    }

    if(frame.op == 0x0002) { /* GET_INFO, page 0 */
        if(frame.length < 1 || frame.payload[0] != 0) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        uint8_t resp[64];
        size_t w = 0;
        resp[w++] = 0; /* page_type */
        mb_put_u16(resp + w, 0xFFFF);
        w += 2; /* next_cursor: none */
        resp[w++] = 1; /* item_count */
        resp[w++] = 1; /* protocol major */
        resp[w++] = 0; /* protocol minor */
#if MB_RELEASE_BUILD
        resp[w++] = 1; /* app major: release 1.0 */
        resp[w++] = 0; /* app minor */
#else
        resp[w++] = 0; /* app major */
        resp[w++] = 11; /* app minor: ...10=C13 ir tx, 11=C15 raw ir + objects */
#endif
        mb_put_u16(resp + w, 87);
        w += 2; /* fw_api_major the SDK was built against */
        mb_put_u16(resp + w, 1);
        w += 2; /* fw_api_minor */
        mb_put_u32(resp + w, furi_kernel_get_tick_frequency());
        w += 4;
        const char* build = BRIDGE_BUILD_ID;
        size_t blen = strlen(build);
        resp[w++] = (uint8_t)blen;
        memcpy(resp + w, build, blen);
        w += blen;
        bridge_send_frame(app, 9, frame.op, MB_OK, frame.correlation, resp, w);
        return;
    }

    if(frame.op == 0x0003) { /* GET_CAPABILITIES (QUERY): op enumeration */
        if(frame.length != 2) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        /* Bootstrap query like GET_INFO: no identity, no lease renewal,
         * no hardware effect. Header: page_type:u8, next_cursor:u16
         * (0xFFFF = end), item_count:u8. Record: op:u16, kind:u8
         * (0=query, 1=action), limit:u32 (op-specific ms bound, 0 =
         * none). Only implemented ops are advertised. */
        static const struct {
            uint16_t op;
            uint8_t kind;
            uint32_t limit;
        } caps[] = {
            {0x0001, 0, 0},
            {0x0002, 0, 0},
            {0x0003, 0, 0},
            {0x0005, 0, 0},
            {0x0006, 0, 0},
            {0x0008, 0, 0},
            {MB_GPIO_OP_CONFIG, 1, MB_GPIO_MAX_HOLD_MS},
            {MB_GPIO_OP_READ, 1, 0},
            {MB_GPIO_OP_WRITE, 1, 0},
            {MB_GPIO_OP_RELEASE, 1, 0},
            {MB_ADC_OP_READ, 1, MB_ADC_SAMPLES_MAX}, /* limit = max samples */
            {MB_NOTIFY_OP, 1, MB_NOTIFY_MAX_EFFECT_MS}, /* limit = max effect ms */
            {MB_IR_OP_RX_START, 1, MB_IR_MAX_TIMEOUT_MS}, /* limit = max timeout ms */
            {MB_IR_OP_TX_DECODED, 1, MB_IR_MAX_TIMEOUT_MS}, /* limit = max timeout ms */
            {MB_OBJ_OP_RX_RAW_START, 1, MB_IR_MAX_TIMEOUT_MS},
            {MB_OBJ_OP_TX_RAW, 1, MB_IR_MAX_TIMEOUT_MS},
            {MB_OBJ_OP_BEGIN, 1, MB_OBJ_MAX_TIMINGS}, /* limit = max timings */
            {MB_OBJ_OP_CHUNK, 1, MB_OBJ_CHUNK_MAX}, /* limit = durations per chunk */
            {MB_OBJ_OP_COMMIT, 1, 0},
            {MB_OBJ_OP_READ, 0, MB_OBJ_READ_MAX}, /* kind 0: query */
            {MB_OBJ_OP_RELEASE, 1, 0},
#if !MB_RELEASE_BUILD
            {BRIDGE_OP_FAKE_RUN, 1, 60000},
#endif
        };
        const uint32_t cap_count = sizeof(caps) / sizeof(caps[0]);
        uint32_t cursor = mb_u16(frame.payload);
        uint32_t start = cursor < cap_count ? cursor : cap_count;
        uint32_t count = cap_count - start;
        if(count > 14) count = 14; /* one page per reply (resp capacity) */
        uint8_t resp[4 + 14 * 7];
        size_t w = 0;
        resp[w++] = 3; /* page_type: capabilities */
        mb_put_u16(resp + w, (start + count < cap_count) ? (uint16_t)(start + count) : 0xFFFF);
        w += 2; /* next_cursor */
        resp[w++] = (uint8_t)count;
        for(uint32_t i = 0; i < count; i++) {
            mb_put_u16(resp + w, caps[start + i].op);
            w += 2;
            resp[w++] = caps[start + i].kind;
            mb_put_u32(resp + w, caps[start + i].limit);
            w += 4;
        }
        bridge_send_frame(app, 9, frame.op, MB_OK, frame.correlation, resp, w);
        return;
    }

    if(frame.op == MB_OBJ_OP_READ) { /* OBJECT_READ (QUERY): C15 */
        if(frame.length != 11) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        /* Session envelope required: a foreign identity learns
         * nothing, not even presence. */
        if(memcmp(frame.identity.boot, app->session.lease.identity.boot, 16) != 0 ||
           memcmp(frame.identity.session, app->session.lease.identity.session, 16) != 0 ||
           frame.identity.generation != app->session.lease.identity.generation) {
            bridge_send_frame(app, 9, frame.op, MB_OUTCOME_UNAVAILABLE, frame.correlation, NULL, 0);
            return;
        }
        uint64_t obj_id = mb_u64(frame.payload);
        uint16_t offset = mb_u16(frame.payload + 8);
        uint8_t count = frame.payload[10];
        if(count == 0 || count > MB_OBJ_READ_MAX) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        MbObjectReadResult rr;
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        MbStatus st = mb_object_read(
            &app->objects, &app->session.lease.identity, obj_id, offset, count, &rr);
        furi_mutex_release(app->exec_mutex);
        if(st != MB_OK) {
            bridge_send_frame(app, 9, frame.op, st, frame.correlation, NULL, 0);
            return;
        }
        uint8_t resp[18 + MB_OBJ_READ_MAX * 4];
        size_t w = 0;
        resp[w++] = rr.present ? 1 : 0;
        resp[w++] = rr.complete ? 1 : 0;
        resp[w++] = rr.from_capture ? 1 : 0;
        resp[w++] = rr.carrier_measured ? 1 : 0;
        mb_put_u16(resp + w, rr.total_count);
        w += 2;
        resp[w++] = rr.returned;
        mb_put_u32(resp + w, rr.crc);
        w += 4;
        mb_put_u32(resp + w, rr.carrier_hz);
        w += 4;
        mb_put_u16(resp + w, rr.duty_permille);
        w += 2;
        resp[w++] = rr.starts_with_mark ? 1 : 0;
        for(uint8_t i = 0; i < rr.returned; i++) {
            mb_put_u32(resp + w, rr.timings[i]);
            w += 4;
        }
        bridge_send_frame(app, 9, frame.op, MB_OK, frame.correlation, resp, w);
        return;
    }

    if(frame.op == 0x0005) { /* GET_RESULT (QUERY): retained outcome */
        if(frame.length != 8) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        /* Recovery may use the all-zero envelope and never renews the
         * lease; any other envelope must match the retained session
         * exactly, generation included. */
        if(!bridge_identity_is_zero(&frame.identity) &&
           (memcmp(frame.identity.boot, app->session.lease.identity.boot, 16) != 0 ||
            memcmp(frame.identity.session, app->session.lease.identity.session, 16) != 0 ||
            frame.identity.generation != app->session.lease.identity.generation)) {
            bridge_send_frame(app, 9, frame.op, MB_OUTCOME_UNAVAILABLE, frame.correlation, NULL, 0);
            return;
        }
        uint64_t seq = mb_u64(frame.payload);
        int idx = mb_ledger_find(&app->ledger, seq);
        uint8_t resp[6 + MB_RESULT_MAX];
        size_t w = 0;
        if(idx < 0) {
            resp[w++] = 0; /* present */
            resp[w++] = 0; /* entry_state */
            mb_put_u16(resp + w, MB_OK);
            w += 2;
            mb_put_u16(resp + w, 0);
            w += 2;
        } else {
            const MbEntry* e = &app->ledger.entry[idx];
            resp[w++] = 1;
            resp[w++] = (uint8_t)e->state;
            mb_put_u16(resp + w, (uint16_t)e->status);
            w += 2;
            mb_put_u16(resp + w, e->result_len);
            w += 2;
            memcpy(resp + w, e->result, e->result_len);
            w += e->result_len;
        }
        bridge_send_frame(app, 9, frame.op, MB_OK, frame.correlation, resp, w);
        return;
    }

    if(frame.op == 0x0006) { /* CANCEL (QUERY): stop one named job */
        if(frame.length != 8) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        /* Valid current identity required; CANCEL consumes no action
         * sequence and is answered under the requester's envelope.
         * mb_same_owner compares ids only, so the generation is
         * checked explicitly here. */
        MbStatus deny = MB_OK;
        if(bridge_identity_is_zero(&frame.identity)) {
            deny = MB_NO_SESSION;
        } else if(memcmp(frame.identity.boot, app->session.lease.identity.boot, 16) != 0 ||
                  memcmp(frame.identity.session, app->session.lease.identity.session, 16) != 0) {
            deny = MB_NO_SESSION;
        } else if(frame.identity.generation != app->session.lease.identity.generation) {
            deny = MB_STALE_CONNECTION;
        }
        if(deny != MB_OK) {
            bridge_send_frame_as(app, &frame.identity, 9, frame.op, deny, frame.correlation, NULL, 0);
            return;
        }
        uint64_t job = mb_u64(frame.payload);
        int idx = mb_ledger_find(&app->ledger, job);
        if(idx < 0) {
            /* An unknown job is never "the current job" (plan 6.5). */
            bridge_send_frame_as(
                app, &frame.identity, 9, frame.op, MB_OUTCOME_UNAVAILABLE, frame.correlation, NULL, 0);
            return;
        }
        const MbEntry* e = &app->ledger.entry[idx];
        if(e->state == MB_TERMINAL) {
            /* Repeated cancellation returns the known state. */
            bridge_send_frame_as(
                app, &frame.identity, 9, frame.op, e->status, frame.correlation, NULL, 0);
            return;
        }
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        bool latched = job == app->active_seq &&
                       mb_executor_request_stop_job(&app->exec, job, MB_STOP_CANCEL);
        MbJobState xs = mb_executor_state(&app->exec);
        furi_mutex_release(app->exec_mutex);
        if(latched || xs == MB_JOB_STOPPING || xs == MB_JOB_TERMINAL) {
            if(latched) {
                app->cancel_requests++;
                bridge_trace(app, MB_EV_STOP_REQUESTED, job, MB_STOP_CANCEL);
            }
            bridge_send_frame_as(
                app, &frame.identity, 9, frame.op, MB_STOP_REQUESTED, frame.correlation, NULL, 0);
        } else {
            bridge_send_frame_as(
                app, &frame.identity, 9, frame.op, MB_OUTCOME_UNAVAILABLE, frame.correlation, NULL, 0);
        }
        return;
    }

    if(frame.op == 0x0008) { /* GET_TRACE (QUERY): paged C02 ring read */
        if(frame.length != 5) {
            bridge_send_frame(app, 9, frame.op, MB_INVALID_ARGUMENT, frame.correlation, NULL, 0);
            return;
        }
        if(!bridge_identity_is_zero(&frame.identity) &&
           (memcmp(frame.identity.boot, app->session.lease.identity.boot, 16) != 0 ||
            memcmp(frame.identity.session, app->session.lease.identity.session, 16) != 0 ||
            frame.identity.generation != app->session.lease.identity.generation)) {
            bridge_send_frame(app, 9, frame.op, MB_OUTCOME_UNAVAILABLE, frame.correlation, NULL, 0);
            return;
        }
        uint32_t after = mb_u32(frame.payload);
        uint32_t limit = frame.payload[4];
        if(limit == 0 || limit > 4) limit = 4; /* section 18.1 page cap */
        MbDiagSnapshot snap;
        mb_diag_snapshot(&app->diag, &snap);
        MbTraceRecord recs[4];
        bool gap = false, more = false;
        uint32_t n = mb_diag_read_after(&app->diag, after, recs, limit, &gap, &more);
        uint8_t resp[19 + 4 * 32];
        size_t w = 0;
        mb_put_u32(resp + w, n ? recs[n - 1].trace_seq : after);
        w += 4; /* next_after_seq */
        resp[w++] = more ? 1 : 0;
        mb_put_u32(resp + w, snap.oldest_seq);
        w += 4;
        mb_put_u32(resp + w, snap.newest_seq);
        w += 4;
        resp[w++] = (uint8_t)n;
        resp[w++] = gap ? 1 : 0;
        for(uint32_t i = 0; i < n; i++) {
            mb_put_u32(resp + w, recs[i].trace_seq);
            w += 4;
            mb_put_u32(resp + w, recs[i].tick);
            w += 4;
            mb_put_u64(resp + w, recs[i].job_id);
            w += 8;
            mb_put_u32(resp + w, recs[i].connection_generation);
            w += 4;
            mb_put_u32(resp + w, recs[i].arg0);
            w += 4;
            mb_put_u32(resp + w, recs[i].arg1);
            w += 4;
            mb_put_u16(resp + w, recs[i].event_code);
            w += 2;
            mb_put_u16(resp + w, recs[i].session_epoch);
            w += 2;
        }
        bridge_send_frame(app, 9, frame.op, MB_OK, frame.correlation, resp, w);
        return;
    }

    app->unsupported++;
    bridge_send_frame(app, 9, frame.op, MB_UNSUPPORTED, frame.correlation, NULL, 0);
}

static bool bridge_uart_acquire(BridgeApp* app) {
    app->expansion = furi_record_open(RECORD_EXPANSION);
    /* Mandatory order (expansion.h): disable BEFORE acquire. */
    expansion_disable(app->expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!app->serial) {
        expansion_enable(app->expansion);
        furi_record_close(RECORD_EXPANSION);
        app->expansion = NULL;
        return false;
    }
    furi_hal_serial_init(app->serial, BRIDGE_BAUD);
    furi_hal_serial_async_rx_start(app->serial, bridge_rx_callback, app, true);
    app->uart_acquired = true;
    return true;
}

static void bridge_uart_release(BridgeApp* app) {
    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
        app->serial = NULL;
    }
    app->uart_acquired = false;
    if(app->expansion) {
        /* Mandatory order: enable right AFTER release. */
        expansion_enable(app->expansion);
        furi_record_close(RECORD_EXPANSION);
        app->expansion = NULL;
    }
}

/* ---- C05 executor plumbing ---- */
static void* bridge_alloc_fn(void* ctx, size_t n) {
    UNUSED(ctx);
    return malloc(n);
}

static void bridge_rng_fill(void* ctx, uint8_t* out, size_t len) {
    UNUSED(ctx);
    furi_hal_random_fill_buf(out, (uint32_t)len);
}

static void bridge_free_fn(void* ctx, void* p) {
    UNUSED(ctx);
    free(p);
}

static int32_t bridge_exec_thread(void* context) {
    BridgeApp* app = context;
    while(!app->exec_thread_stop) {
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        mb_executor_service(&app->exec, furi_get_tick());
        furi_mutex_release(app->exec_mutex);
        app->exec_heartbeats++;
        furi_delay_ms(10);
    }
    return 0;
}

/* Persist final counters so the Pi can read them back over RPC after
 * the expansion link is restored (spike diagnostic channel). */
static void bridge_write_result(BridgeApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/apps_data/muse_bridge");
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, "/ext/apps_data/muse_bridge/result.txt", FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[1024];
        uint32_t run_ms = (furi_get_tick() - app->started_tick) * 1000U / furi_kernel_get_tick_frequency();
        MbDiagSnapshot ts;
        mb_diag_snapshot(&app->diag, &ts);
        MbTraceRecord tail[4];
        bool gap = false, more = false;
        uint32_t from = ts.newest_seq >= 4 ? ts.newest_seq - 4 : 0;
        uint32_t tn = mb_diag_read_after(&app->diag, from, tail, 4, &gap, &more);
        uint32_t ev[4] = {0, 0, 0, 0};
        for(uint32_t i = 0; i < tn && i < 4; i++) ev[i] = tail[i].event_code;
        MbIrSummary irs;
        mb_ir_last_summary(&irs);
        MbIrTxSummary irtxs;
        mb_ir_tx_last_summary(&irtxs);
        MbIrRawSummary irraws;
        mb_ir_raw_last_summary(&irraws);
        MbIrTxRawSummary irrawtxs;
        mb_ir_tx_raw_last_summary(&irrawtxs);
        int len = snprintf(
            buf,
            sizeof(buf),
            "build=%s rx_bytes=%lu rx_errors=%lu frames=%lu crc_errors=%lu codec_errors=%lu "
            "oversize=%lu partial_timeouts=%lu ring_drops=%lu unsupported=%lu "
            "exec_starts=%lu exec_terminals=%lu fake_exec=%lu fake_terminals=%lu fake_status=%lu "
            "cancels=%lu gpio_cfg=%lu gpio_wr=%lu gpio_rd=%lu gpio_rel=%lu gpio_exp=%lu gpio_claims=%lu "
            "adc_rd=%lu adc_smp=%lu ntfy=%lu ir_dec=%lu ir_emit=%lu ir_drop=%lu "
            "ir_tx_sup=%lu ir_tx_sent=%lu ir_raw_cap=%lu ir_raw_over=%lu "
            "ir_rawtx_sup=%lu ir_rawtx_sent=%lu ir_rawtx_tot=%lu "
            "data_gen=%lu data_enq=%lu data_drop=%lu data_consumed=%lu exec_beats=%lu "
            "heap_sf=%lu heap_smin=%lu heap_sblk=%lu heap_ef=%lu heap_emin=%lu heap_eblk=%lu "
            "rx_bytes_1s=%lu rx_errors_1s=%lu "
            "trace_new=%lu trace_cnt=%lu trace_ovw=%lu tlast=%lu,%lu,%lu,%lu "
            "link=%u gen=%lu sacc=%llu siss=%llu high_water=%llu active=%llu run_ms=%lu\n",
            BRIDGE_BUILD_ID,
            (unsigned long)app->rx_bytes,
            (unsigned long)app->rx_errors,
            (unsigned long)app->valid_frames,
            (unsigned long)app->crc_errors,
            (unsigned long)app->codec_errors,
            (unsigned long)app->oversize_frames,
            (unsigned long)app->partial_timeouts,
            (unsigned long)app->ring_drops,
            (unsigned long)app->unsupported,
            (unsigned long)app->exec.starts,
            (unsigned long)app->exec.terminals,
            (unsigned long)mb_fake_execution_count(),
            (unsigned long)app->fake_terminals,
            (unsigned long)app->fake_last_status,
            (unsigned long)app->cancel_requests,
            (unsigned long)app->gpio_state.configs,
            (unsigned long)app->gpio_state.writes,
            (unsigned long)app->gpio_state.reads,
            (unsigned long)app->gpio_state.releases,
            (unsigned long)app->gpio_state.expiries,
            (unsigned long)mb_gpio_claimed_count(&app->gpio_state),
            (unsigned long)app->adc_state.reads,
            (unsigned long)app->adc_state.samples_total,
            (unsigned long)app->notify_state.effects_played,
            (unsigned long)irs.decoded,
            (unsigned long)irs.emitted,
            (unsigned long)irs.dropped,
            (unsigned long)irtxs.supplied,
            (unsigned long)irtxs.sent,
            (unsigned long)irraws.captures_total,
            (unsigned long)irraws.over_cap_total,
            (unsigned long)irrawtxs.supplied,
            (unsigned long)irrawtxs.sent,
            (unsigned long)irrawtxs.tx_total,
            (unsigned long)app->exec.generated,
            (unsigned long)app->exec.enqueued,
            (unsigned long)app->exec.dropped,
            (unsigned long)app->exec.consumed,
            (unsigned long)app->exec_heartbeats,
            (unsigned long)app->heap_start_free,
            (unsigned long)app->heap_start_min,
            (unsigned long)app->heap_start_block,
            (unsigned long)app->heap_end_free,
            (unsigned long)app->heap_end_min,
            (unsigned long)app->heap_end_block,
            (unsigned long)app->rx_bytes_1s,
            (unsigned long)app->rx_errors_1s,
            (unsigned long)ts.newest_seq,
            (unsigned long)ts.count,
            (unsigned long)ts.overwrites,
            (unsigned long)ev[0],
            (unsigned long)ev[1],
            (unsigned long)ev[2],
            (unsigned long)ev[3],
            (unsigned)app->session.lease.state,
            (unsigned long)app->session.lease.identity.generation,
            (unsigned long long)app->session.lease.accepted_counter,
            (unsigned long long)app->session.lease.issued_counter,
            (unsigned long long)app->ledger.high_water,
            (unsigned long long)app->active_seq,
            (unsigned long)run_ms);
        if(len > 0) storage_file_write(file, buf, (uint16_t)len);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

int32_t muse_bridge_app(void* context) {
    UNUSED(context);
    BridgeApp* app = malloc(sizeof(BridgeApp));
    memset(app, 0, sizeof(BridgeApp));
    app->started_tick = furi_get_tick();
    furi_hal_random_fill_buf(app->boot_id, sizeof(app->boot_id));
    mb_diag_init(&app->diag);
    bridge_trace(app, MB_EV_APP_START, 0, 0);

    /* C06 session with the plan's fixed timing defaults. */
    {
        uint32_t hz = furi_kernel_get_tick_frequency();
        MbSessionConfig scfg = {0};
        mb_ticks_from_ms(1500, hz, &scfg.timing.suspect);
        mb_ticks_from_ms(3000, hz, &scfg.timing.expiry);
        mb_ticks_from_ms(1000, hz, &scfg.timing.proof_age);
        mb_ticks_from_ms(2000, hz, &scfg.pending_ttl_ticks);
        scfg.challenge_interval_ms = 500;
        scfg.proof_max_age_ms = 1000;
        scfg.suspect_ms = 1500;
        scfg.lease_ms = 3000;
        scfg.max_payload = 176;
        mb_session_init(&app->session, app->boot_id, &scfg, bridge_rng_fill, NULL);
        app->prev_link = MB_WAITING;
    }

    uint32_t partial_ticks = 0;
    if(!mb_ticks_from_ms(BRIDGE_PARTIAL_MS, furi_kernel_get_tick_frequency(), &partial_ticks) ||
       !mb_rx_init(&app->rx, partial_ticks)) {
        free(app);
        return -1;
    }
    app->rx_stream = furi_stream_buffer_alloc(2048, 1);

    /* C05: executor + its thread come up before the UART so the job
     * plumbing is live for the whole session (no job runs until the
     * controller admits the internal qualification job below). */
    app->exec_alloc.alloc = bridge_alloc_fn;
    app->exec_alloc.free = bridge_free_fn;
    app->exec_alloc.ctx = NULL;
    mb_executor_init(&app->exec, &app->exec_alloc);
    app->exec_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    /* C10 GPIO: bind the claim table + FAP HAL before the executor
     * thread starts (GPIO jobs touch them only under exec_mutex). */
    mb_gpio_init(&app->gpio_state);
    app->gpio_hal = (MbGpioHal){
        .apply_config = bridge_gpio_hal_config,
        .write = bridge_gpio_hal_write,
        .read = bridge_gpio_hal_read,
        .restore = bridge_gpio_hal_restore,
        .ctx = NULL};
    mb_gpio_module_bind(&app->gpio_state, &app->gpio_hal);
    /* C11: ADC + notification modules bind the same way. */
    mb_adc_init(&app->adc_state);
    app->adc_hal = (MbAdcHal){
        .prepare_pin = bridge_adc_prepare_pin,
        .restore_pin = bridge_adc_restore_pin,
        .read_sample = bridge_adc_read_sample,
        .ctx = NULL};
    mb_adc_module_bind(&app->adc_state, &app->adc_hal);
    mb_notify_init(&app->notify_state);
    app->notify_hal = (MbNotifyHal){.play = bridge_notify_play, .ctx = app};
    mb_notify_module_bind(&app->notify_state, &app->notify_hal);
    app->ir_hal = (MbIrHal){
        .rx_start = bridge_ir_rx_start,
        .rx_start_raw = bridge_ir_rx_start_raw,
        .rx_stop = bridge_ir_rx_stop,
        .tx_start = bridge_ir_tx_start,
        .tx_stop = bridge_ir_tx_stop,
        .ctx = app};
    mb_ir_module_bind(&app->ir_hal);
    /* C15: object store + raw TX pin discipline. */
    mb_object_store_init(&app->objects);
    mb_object_module_bind(&app->objects, &app->session.lease.identity, bridge_now_ms);
    MbIrTxRawBind raw_bind = {.pin = bridge_obj_pin, .unpin = bridge_obj_unpin, .ctx = app};
    mb_ir_tx_raw_bind(&raw_bind);
    app->notification = furi_record_open(RECORD_NOTIFICATION);
    app->exec_thread = furi_thread_alloc_ex("MbExec", 2048, bridge_exec_thread, app);
    furi_thread_start(app->exec_thread);

    app->heap_start_free = memmgr_get_free_heap();
    app->heap_start_min = memmgr_get_minimum_free_heap();
    app->heap_start_block = memmgr_heap_get_max_free_block();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, bridge_draw_callback, app);
    view_port_input_callback_set(app->view_port, bridge_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    if(!bridge_uart_acquire(app)) {
        FURI_LOG_E("muse_bridge", "serial acquire failed");
        bridge_trace(app, MB_EV_FAULT, 0, 1);
    } else {
        bridge_trace(app, MB_EV_UART_ACQUIRED, 0, 0);
    }

    uint32_t last_shown = 0xFFFFFFFF;
    while(!app->exit_requested) {
        /* Poll lease expiry before any queued proof is considered;
         * record link-state transitions in the trace. */
        uint32_t now_ms0 = (furi_get_tick() - app->started_tick) * 1000U / furi_kernel_get_tick_frequency();
        MbLink link_now = mb_session_poll(&app->session, furi_get_tick());
        if(link_now != app->prev_link) {
            if(link_now == MB_SUSPECT) {
                bridge_trace(app, MB_EV_LINK_SUSPECT, 0, 0);
            } else if(link_now == MB_EXPIRED) {
                bridge_trace(app, MB_EV_LEASE_EXPIRED, 0, 0);
                /* At lease expiry a running job stops (plan 7.4); its
                 * terminal still publishes to the retained ledger. */
                furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
                if(mb_executor_busy(&app->exec)) {
                    mb_executor_request_stop(&app->exec, MB_STOP_LINK);
                    bridge_trace(app, MB_EV_STOP_REQUESTED, app->active_seq, MB_STOP_LINK);
                }
                /* Link loss restores every bridge-owned pin (plan
                 * 7.1): nothing may stay driven on a dead lease. */
                uint32_t dropped_pins = mb_gpio_release_all(&app->gpio_state, &app->gpio_hal);
                furi_mutex_release(app->exec_mutex);
                if(dropped_pins) {
                    bridge_trace(app, MB_EV_RESOURCE_RELEASED, 0, dropped_pins);
                }
            } else if(link_now == MB_LIVE) {
                bridge_trace(app, MB_EV_SESSION_LIVE, 0, 0);
            }
            app->prev_link = link_now;
        }
        /* C15: when the session identity is replaced (a new
         * handshake — including a resume, which mints a fresh
         * identity), objects owned by the previous identity are
         * purged once the executor is idle. Lease expiry alone does
         * not change the identity and frees nothing. */
        {
            const MbIdentity* cur_id = &app->session.lease.identity;
            if(!app->obj_seen_valid) {
                app->obj_seen_identity = *cur_id;
                app->obj_seen_valid = true;
            } else if(memcmp(&app->obj_seen_identity, cur_id, sizeof(MbIdentity)) != 0) {
                furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
                if(!mb_executor_busy(&app->exec)) {
                    mb_object_purge_session(&app->objects, &app->obj_seen_identity);
                    app->obj_seen_identity = *cur_id;
                }
                furi_mutex_release(app->exec_mutex);
            }
        }
        /* Bench signals: while anything is listening, flash the
         * blue LED about once a second (non-blocking; the blink
         * sequence extinguishes itself). */
        if(app->signal_listen && !app->signal_muted &&
           now_ms0 - app->signal_last_blink >= 900) {
            app->signal_last_blink = now_ms0;
            notification_message(app->notification, &bridge_seq_listen_blink);
        }
        /* While linked, offer a fresh heartbeat challenge every 500 ms.
         * The lease records creation before enqueueing; a failed send
         * grants nothing — only a fresh echo renews the lease. */
        if((link_now == MB_LIVE || link_now == MB_SUSPECT) &&
           now_ms0 - app->last_challenge_ms >= 500) {
            app->last_challenge_ms = now_ms0;
            uint64_t counter = mb_session_challenge(&app->session, furi_get_tick());
            if(counter) {
                uint8_t payload[8];
                mb_put_u64(payload, counter);
                bridge_send_frame_as(
                    app, &app->session.lease.identity, 5, 0, MB_OK, 0, payload, sizeof(payload));
            }
        }

        /* Drain the byte ring into the collector. */
        uint8_t chunk[64];
        size_t got;
        while((got = furi_stream_buffer_receive(app->rx_stream, chunk, sizeof(chunk), 0)) > 0) {
            for(size_t i = 0; i < got; i++) {
                MbRxEvent ev = mb_rx_byte(&app->rx, chunk[i], furi_get_tick());
                if(ev == MB_RX_FRAME) {
                    bridge_handle_frame(app, app->rx.encoded, app->rx.ready_len, furi_get_tick());
                } else if(ev == MB_RX_OVERSIZE) {
                    app->oversize_frames++;
                    bridge_trace(app, MB_EV_FRAME_DROP, 0, 3);
                } else if(ev == MB_RX_TIMEOUT) {
                    app->partial_timeouts++;
                    bridge_trace(app, MB_EV_FRAME_DROP, 0, 4);
                }
            }
        }
        if(app->ring_overflowed) {
            app->ring_overflowed = false;
            mb_rx_poison(&app->rx);
        }
        if(mb_rx_expire(&app->rx, furi_get_tick())) {
            app->partial_timeouts++;
            bridge_trace(app, MB_EV_FRAME_DROP, 0, 4);
        }

        /* C07 controller glue: forward the wire job's data as EVENTs
         * and publish its single terminal outcome to the ledger BEFORE
         * the COMPLETE frame (the retained result outlives the send). */
        MbDataRecord recs[4];
        size_t rec_count = 0;
        uint64_t term_job = 0;
        MbStatus term_status = MB_OK;
        bool have_terminal = false;
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        MbDataRecord rec;
        while(rec_count < 4 && mb_executor_data_poll(&app->exec, &rec)) {
            recs[rec_count++] = rec;
        }
        if(mb_executor_poll_terminal(&app->exec, &term_job, &term_status, NULL)) {
            have_terminal = true;
        }
        bool exec_busy = mb_executor_busy(&app->exec);
        if(app->back_pressed) {
            if(exec_busy && !app->stop_sent) {
                mb_executor_request_stop(&app->exec, MB_STOP_LOCAL);
                app->stop_sent = true;
                bridge_trace(app, MB_EV_STOP_REQUESTED, 0, MB_STOP_LOCAL);
            } else if(!exec_busy) {
                app->exit_requested = true;
            }
        }
        /* Once nothing is running, the stop obligation a revocation
         * left behind has been transferred and discharged. */
        if(!exec_busy && app->session.lease.revocation_pending) {
            app->session.lease.revocation_pending = false;
        }
        furi_mutex_release(app->exec_mutex);

        if(app->active_seq) {
            for(size_t i = 0; i < rec_count; i++) {
                bridge_send_event(app, &recs[i]);
            }
        }
        if(have_terminal) {
            app->fake_terminals++;
            app->fake_last_status = (uint32_t)term_status;
            bridge_trace(app, MB_EV_JOB_TERMINAL, term_job, (uint32_t)term_status);
            if(term_job == app->active_seq && app->active_seq != 0) {
                uint16_t done_op = app->active_op ? app->active_op : BRIDGE_OP_FAKE_RUN;
                uint8_t result[32];
                size_t result_len;
                if(done_op == BRIDGE_OP_FAKE_RUN) {
                    mb_put_u16(result, (uint16_t)term_status);
                    mb_put_u32(result + 2, app->exec.generated - app->admit_generated);
                    mb_put_u32(result + 6, app->exec.dropped - app->admit_dropped);
                    result_len = 10;
                } else {
                    result_len = bridge_gpio_result(app, done_op, term_status, result);
                }
                mb_ledger_finish(&app->ledger, term_job, term_status, result, result_len);
                bridge_send_frame_as(
                    app, &app->session.lease.identity, 12, done_op,
                    term_status, term_job, result, result_len);
                /* Module stopped, cleanup done, outcome durable, slot
                 * released: the hardware is quiescent (C08). */
                bridge_trace(app, MB_EV_HW_QUIESCENT, term_job, (uint32_t)term_status);
                app->active_seq = 0;
                app->active_op = 0;
            }
        }

        /* C10: output hold deadlines expire even while the session is
         * healthy — the pin returns to its analog default and the
         * claim ends (observed externally, never re-armed locally). */
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        mb_gpio_service(&app->gpio_state, &app->gpio_hal, furi_get_tick());
        furi_mutex_release(app->exec_mutex);

        uint32_t shown = app->valid_frames + app->crc_errors + app->rx_bytes + app->fake_terminals;
        if(shown != last_shown) {
            last_shown = shown;
            view_port_update(app->view_port);
        }
        uint32_t elapsed_ms = (furi_get_tick() - app->started_tick) * 1000U / furi_kernel_get_tick_frequency();
        if(!app->rx_snap_taken && elapsed_ms >= 1000) {
            app->rx_bytes_1s = app->rx_bytes;
            app->rx_errors_1s = app->rx_errors;
            app->rx_snap_taken = true;
        }
#if !MB_RELEASE_BUILD
        if(elapsed_ms > BRIDGE_AUTO_EXIT_MS) break;
#endif
        furi_delay_ms(20);
    }

    /* If a job is still active, stop it and let the executor finish
     * cleanup before its thread goes away (§9.5). */
    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    if(mb_executor_busy(&app->exec)) {
        mb_executor_request_stop(&app->exec, MB_STOP_LOCAL);
    }
    furi_mutex_release(app->exec_mutex);
    for(int i = 0; i < 100; i++) {
        furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
        bool busy = mb_executor_busy(&app->exec);
        furi_mutex_release(app->exec_mutex);
        if(!busy) break;
        furi_delay_ms(20);
    }
    /* Exit restores every bridge-owned pin (analog default). */
    furi_mutex_acquire(app->exec_mutex, FuriWaitForever);
    mb_gpio_release_all(&app->gpio_state, &app->gpio_hal);
    furi_mutex_release(app->exec_mutex);
    app->exec_thread_stop = true;
    furi_thread_join(app->exec_thread);
    furi_thread_free(app->exec_thread);
    mb_executor_deinit(&app->exec);
    furi_mutex_free(app->exec_mutex);

    bridge_uart_release(app);
    bridge_trace(app, MB_EV_RESOURCE_RELEASED, 0, 0);
    bridge_trace(app, MB_EV_APP_EXIT, 0, 0);

    app->heap_end_free = memmgr_get_free_heap();
    app->heap_end_min = memmgr_get_minimum_free_heap();
    app->heap_end_block = memmgr_heap_get_max_free_block();

    bridge_write_result(app);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    if(app->notification) {
        furi_record_close(RECORD_NOTIFICATION);
        app->notification = NULL;
    }
    furi_stream_buffer_free(app->rx_stream);
    free(app);
    return 0;
}
