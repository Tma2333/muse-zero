# Changelog

## 2026-10-08 — P1-A complete: bridge 1.0

The first usable release is qualified end-to-end on stock firmware
1.4.3:

- **Finite IR transmit** (decoded NEC, exactly one frame per action):
  a verbatim retry after a dropped reply replays the stored result
  and emits nothing — proven by an independent Pi-side IR receiver
  counting physical frames, not by firmware counters
- **Release qualification**: the full L01–L25 link fault-injection
  matrix (dropped/duplicated/corrupted frames, lease expiry, client
  and FAP restarts), 100 start/stop cycles per hardware module with
  no heap fragmentation growth, a 30-minute continuous-receive soak
  with injected link expiries, and smoke tests run against the
  release binary itself
- **Release build**: development test operation compiled out of
  dispatch and the capability table; capabilities advertise exactly
  the qualified operations. Release notes and a Pi-client
  conformance report live in `muse_bridge/`
  (`RELEASE_NOTES_1.0.md`, `CONFORMANCE_P1A.md`); the host-native
  suite now runs as one command (`tests/native/run_all.sh`)

Next increments (same qualification discipline each): raw IR, NFC
identification, LF RFID read, Sub-GHz receive, scoped storage.

## 2026-10-04 — Initial public snapshot

First working Muse ↔ Flipper Zero hardware bridge, verified on stock
Flipper firmware 1.4.3 (Raspberry Pi host, UART on header pins 13/14):

- Session/lease protocol over COBS + CRC-32 framing at 230400 baud,
  with exactly-once action execution, cancellation, absolute deadlines,
  and a wire-readable diagnostic trace
- Hardware modules qualified on device: GPIO (pins 2–7), ADC
  (pins 2/3/4/7), finite notifications (LED/beep/vibration), decoded
  NEC IR receive with ordered, drop-accounted events
- Pi tooling: expansion/RPC transport + driver, Python wire codec,
  hardware-in-the-loop harness, host-native test suite
- Protocol documented in `muse_bridge/protocol.md`

Not in this build (by design): any transmit/emulation feature, tag
writing. Roadmap lives in the README.
