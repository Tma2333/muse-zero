# Changelog

## 2026-10-08 — Raw IR + bench signals

- **Raw IR capture and replay**: a listening job stores the first
  burst's microsecond timings as a bounded object (512 timings,
  CRC-checked); the object can be read back in pages and replayed
  as exactly one finite train at its declared carrier settings.
  Captures report carrier as *unknown* rather than inventing a
  measurement, over-cap captures are retained only as explicitly
  incomplete diagnostics that can never transmit, and partially
  uploaded objects can never emit. Verified on hardware: a real
  remote capture decoded independently on the host, and both
  captured and uploaded waveforms counted frame-by-frame by the
  bench receiver (exactly one frame each)
- **Bounded object transfer**: chunked upload with contiguous
  ranges, identical-range re-acknowledgment for lost replies, and
  commit-time validation (count, per-duration bounds, total,
  checksum). Objects are session-owned and purged when a session
  is replaced
- **Bench signals**: the device announces its own listening state
  — a double beep when a listening period opens and closes and a
  flashing LED while it lasts — generalized to every listening
  job and drivable from the host as notification effects, with a
  mute gate for long cycle tests. Designed at the bench after a
  chat-timed test missed its windows; with the signals, the same
  capture landed in under four seconds

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
