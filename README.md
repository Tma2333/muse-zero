# Muse Zero

**Give an AI assistant hands that can touch the physical world — safely,
provably, one verified capability at a time.**

Muse Zero connects Muse to a Flipper Zero through a Raspberry Pi
(Flipper Zero + Pi Zero + Muse). It is one project with a center and a
growing shell of assets, instructions, and scripts around it.

## Projects

| Section | What it is |
|---|---|
| [`muse_bridge/`](muse_bridge/) | **The center**: the Muse Bridge app on the Flipper (`muse_bridge.c`, `core/`, `transport/`, `modules/`, `tests/`, `tools/`), its Pi-side half (`pi_host/`), and the wire spec (`protocol.md`). |
| [`jolly_animations/`](jolly_animations/) | Drop-in desktop animation assets — the complete "Jolly" pack with previews, ready to copy onto a Flipper. |

Setup guides are split the same way: [`SETUP.md`](SETUP.md) covers
the general Muse ↔ Flipper link (Pi, wiring, stock RPC); the bridge
app and the animations each have their own SETUP in their folders.

## Why a Pi in the middle?

The Flipper's official WiFi devboard has no Bluetooth, and Muse gadgets
pair over BLE — so the Pi is the bridge in both senses. It hosts the
Muse Gadgets agent so Muse can reach it, and it speaks serial
protocols to the Flipper: the stock Expansion Module Protocol (over USB)
for setup/storage, and a dedicated COBS+CRC UART protocol to the bridge
app for hardware commands. The same design collapses onto a Pi Zero 2 W
backpack plugged onto the Flipper's header.

## The bridge, in one paragraph

The bridge app exposes the Flipper's hardware — GPIO, ADC, IR,
notifications, and (in progress) NFC, LF RFID, iButton and Sub-GHz —
behind a session protocol with leases, heartbeat liveness,
cancellation, absolute deadlines, and exactly-once execution
(duplicate requests replay their results; nothing runs twice).
One client owns the hardware at a time; if the link dies, the lease
expires and any running job stops. Every capability joins the
advertised list only after being qualified on real hardware against a
physical observer.

## Status (October 2026)

Working and hardware-verified on stock Flipper firmware 1.4.3:

- Session handshake with leases and heartbeat-challenge liveness
- Exactly-once actions with cancel, deadlines, and post-restart
  outcome lookup; a wire-readable diagnostic trace
- GPIO output/input on header pins 2–7 (auto-release on link loss)
- ADC reads (multi-sample averaged) on pins 2/3/4/7
- Finite notifications (LED flash, beep, vibration)
- Decoded IR receive (NEC), ordered and drop-accounted

Deliberately **not** present: IR/RFID/NFC/Sub-GHz transmission, tag
writing, and credential copying. Those arrive one at a time, each
behind a physical verification fixture and explicit human approval.
See [ROADMAP.md](ROADMAP.md).

## Quick start

Point your Muse at [SETUP.md](SETUP.md) — written to be followed
step-by-step, by a human or an AI agent with shell access to the Pi.
Just want the dancing robot on your Flipper? See
[jolly_animations/README.md](jolly_animations/README.md).

## Safety posture

- One owner, expiring leases, stop-on-silence.
- Physical claims get physical proof (witness wires, meters, real
  remotes), never assumptions.
- The app releases everything it claimed on exit, and stock Flipper
  apps keep working.

## License

MIT. See [LICENSE](LICENSE).
