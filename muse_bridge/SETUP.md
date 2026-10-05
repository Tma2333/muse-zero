# Setup — Muse Bridge app

The Muse Bridge is a Flipper app (`.fap`) that exposes the Flipper's
hardware — GPIO, ADC, notifications, IR receive, and later NFC/RFID/
iButton/Sub-GHz — to the Pi over a dedicated UART protocol with
sessions, leases, cancellation, deadlines, and exactly-once execution.

**Prerequisite**: the general rig from [`../SETUP.md`](../SETUP.md)
(Pi paired, three wires, `flipper_driver.py info` answering).

## 1. Build

```bash
pip install ufbt               # or a project venv
cd muse_bridge
ufbt update --branch=1.4.3     # match your firmware branch
ufbt build                     # → dist/muse_bridge.fap
```

Sanity-check the portable core anywhere (no Flipper needed):

```bash
cd tests/native && make run    # COBS/CRC codec + session gates
```

## 2. Install

Copy `dist/muse_bridge.fap` to `/ext/apps/Tools/muse_bridge.fap` on
the Flipper — qFlipper, the Pi driver (`flipper_driver.py put`), or
your usual file path. Launch it from Apps → Tools → Muse Bridge, or
remotely via the driver.

While it runs it owns the UART: stock RPC pauses, and returns when
the app exits (Back, or the app's idle auto-exit in this development
build — currently 90 s).

## 3. Talk to it

With the app running, from the Pi:

```bash
cd tools
FLIPPER_RPC_DIR=~/flipper-rpc ~/flipper-rpc/.venv/bin/python bridge_hil.py --case C04
```

Cases are progressive (bootstrap → sessions → actions → cancellation
→ recovery → GPIO → ADC/notify → IR receive). Work upward only as
your bench fixtures allow:

- **GPIO**: a witness wire from a Flipper pin (2–7) to a Pi GPIO
- **ADC**: a known-safe voltage (ground, or ≤3.3 V) on pins 2/3/4/7
- **IR receive**: any NEC remote

## What to know

- Sessions are RAM-only — relaunching the app resets them, by design.
- `GET_CAPABILITIES` is the truth about what this build supports;
  transmit features are absent (not merely disabled).
- Session/lease/cancel semantics and every frame layout:
  [`protocol.md`](protocol.md).
