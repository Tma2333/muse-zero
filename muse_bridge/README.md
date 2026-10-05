# Muse Bridge

The Muse Bridge is the center of Muse Zero: a small app on the Flipper
Zero plus a Pi-side client, connected by a wire protocol designed to be
safe to program against. It lets Muse — running on the Raspberry Pi —
sense and act through the Flipper's hardware, with sessions, leases,
and exactly-once execution wrapped around every command.

```
   Muse ──► Raspberry Pi ──UART──► Flipper Zero
              pi_host/               muse_bridge.c + core/ + modules/
```

## What's in here

| Path | What |
|---|---|
| `muse_bridge.c`, `core/`, `transport/`, `modules/` | The Flipper app (FAP) for stock firmware 1.4.x |
| `pi_host/` | Pi-side Expansion/RPC transport and driver |
| `protocol.md` | The wire protocol, frame by frame |
| `tests/native/` | Host-native tests for the portable core (run anywhere) |
| `tools/` | Python codec + hardware-in-the-loop harness |
| `SETUP.md` | Build, install, and self-test the app |

The general Muse ↔ Flipper link (pairing the Pi, wiring, stock RPC)
lives in the repo's root [`SETUP.md`](../SETUP.md).

## What it exposes

Behind a session handshake with an expiring lease, one client at a
time can:

- configure, read, write, and release GPIO header pins (auto-released
  when the link or session ends),
- read analog voltages with multi-sample averaging,
- fire finite notifications (LED flash, beep, vibration) that always
  complete on their own,
- receive decoded infrared remote signals as an ordered,
  drop-accounted event stream,
- ask what actually happened: cancellation, absolute deadlines, and a
  diagnostic trace readable over the wire.

Every action executes exactly once — a duplicate request replays its
stored result instead of acting again — and outcomes stay readable
after cancellation, link loss, or an app restart.

## What it deliberately doesn't do

No transmissions (IR/Sub-GHz), no NFC/RFID emulation, no tag writing.
Those arrive one at a time, each proven against an independent
receiver and gated behind explicit human approval; the advertised
capability list (`GET_CAPABILITIES` in the protocol) only ever names
what's verified on hardware.
