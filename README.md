# Muse Zero

**Give an AI assistant hands that can touch the physical world — safely,
provably, one verified capability at a time.**

Muse Zero connects Muse to a Flipper Zero through a Raspberry Pi
(Flipper Zero + Pi Zero + Muse). It is one project with a center and a
growing shell of assets, instructions, and scripts around it.

## The map

| Section | What it is |
|---|---|
| [`bridge/`](bridge/) | **The center**: the comm link between Muse and the Flipper. `bridge/flipper-app/` is the Muse Bridge app running on the Flipper; `bridge/pi-host/` is the Pi-side transport and driver; `bridge/protocol.md` is the wire protocol between them. |
| [`animations/`](animations/) | Drop-in desktop animation assets — the complete "Jolly" pack, ready to copy onto a Flipper, plus the tools that make more. |
| [`workflows/`](workflows/) | Guided procedures an AI assistant can follow with this hardware (coming). |
| [`flipper-scripts/`](flipper-scripts/) | Programs and content that run on the Flipper side (coming). |
| [`pi-scripts/`](pi-scripts/) | Scripts that run on the Pi to drive hardware and workflows (coming). |

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
[animations/README.md](animations/README.md).

## Safety posture

- One owner, expiring leases, stop-on-silence.
- Physical claims get physical proof (witness wires, meters, real
  remotes), never assumptions.
- The app releases everything it claimed on exit, and stock Flipper
  apps keep working.

## License

MIT. See [LICENSE](LICENSE).
