# Muse Zero

**Give an AI assistant hands that can touch the physical world — safely,
provably, one verified capability at a time.**

Muse Zero connects Muse to a Flipper Zero through a Raspberry Pi
(Flipper Zero + Pi Zero + Muse). Muse runs on the Pi as a gadget; the Pi
talks to the Flipper over three UART wires; a small Flipper app
("Muse Bridge") exposes the Flipper's hardware — GPIO, ADC, IR,
notifications, and (in progress) NFC, LF RFID, iButton and Sub-GHz — behind
a session-based protocol with leases, cancellation, and exactly-once
execution. Muse can then *act* in the physical world with the same care
it applies to software: every capability is qualified on real hardware
before it is advertised, transmit features are gated behind explicit
human confirmation, and everything the bridge claims to have done can be
audited after the fact.

## Why a Pi in the middle?

The Flipper's official WiFi devboard has no Bluetooth, and Muse gadgets
pair over BLE — so the Pi is the bridge in both senses:

- It hosts the Muse Gadgets agent (Linux SDK) so Muse can reach it.
- It speaks the Flipper's Expansion Module Protocol (protobuf RPC over
  USB for setup/storage) and a dedicated COBS+CRC UART protocol to the
  bridge app for hardware commands.
- The same design collapses onto a Pi Zero 2 W backpack that plugs
  straight onto the Flipper's header: one cable to charge, everything
  else on the pins.

## What's in the box

| Path | What |
|---|---|
| `muse_bridge.c`, `core/`, `transport/`, `modules/` | The Flipper app (FAP), built with uFBT against official firmware 1.4.x |
| `pi/` | Pi-side transport (`expansion_transport.py`) + RPC driver (`flipper_driver.py`) |
| `tools/` | Python wire codec and the hardware-in-the-loop test harness |
| `tests/native/` | Host-native unit tests (build with `make`, run anywhere) |
| `docs/protocol.md` | The wire protocol, documented frame by frame |

## Status (October 2026)

Working and hardware-verified on stock Flipper firmware 1.4.3:

- Session handshake with leases, heartbeat-challenge liveness, and
  exactly-once action execution (duplicate requests replay results,
  never re-execute)
- GPIO output/input on header pins 2–7 with automatic release on
  disconnect, cancel, or lease expiry
- ADC reads (multi-sample averaged) on pins 2/3/4/7
- Finite notifications (LED flash, beep, vibration)
- Decoded IR receive (NEC) with ordered, drop-accounted event streams

Deliberately **not** present: IR/RFID/NFC/Sub-GHz transmission, tag
writing, and credential copying. Output features are added one at a time,
each behind a physical verification fixture and explicit human approval.
See [ROADMAP.md](ROADMAP.md).

## Quick start

Point your Muse at [SETUP.md](SETUP.md) — it is written to be followed
step-by-step, by a human or an AI agent with shell access to the Pi.
Short version: flash a Pi, pair it as a Muse gadget, wire three jumper
wires, build the FAP with uFBT, install it, and run the self-tests.

## Safety posture

- One client owns the hardware at a time; leases expire if the link dies,
  and expiry stops any running job.
- Every pins-and-radios claim is tested against a physical observer
  (a wired witness, a meter, a real remote) — never assumed.
- The app never touches radios or storage outside its scoped commands,
  releases everything it claimed on exit, and leaves stock Flipper apps
  fully working.

## License

MIT. See [LICENSE](LICENSE).
