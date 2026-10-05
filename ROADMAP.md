# Roadmap

High-level only; every item lands as a module behind the session/
executor foundation, qualified on real hardware before it joins the
advertised capability list.

## Done

- [x] Session foundation — handshake, lease with heartbeat liveness,
      single-owner concurrency, same-client resume
- [x] Exactly-once action execution with cancellation, deadlines,
      and post-restart outcome lookup
- [x] Diagnostic trace ring readable over the wire
- [x] GPIO (pins 2–7): configure/read/write/release, hold deadlines,
      automatic release on link/session loss
- [x] ADC reads (pins 2/3/4/7), multi-sample averaging
- [x] Finite notifications: LED flash, beep, vibration
- [x] Decoded IR receive (NEC) with sequenced event streams

## Next

- [ ] One verified finite IR transmission (needs a bench IR receiver
      fixture to independently confirm exactly one frame)
- [ ] Release qualification: endurance cycles, long receive/fault
      soak, release build without development hooks, stock-app smoke
      tests, pinned build artifact
- [ ] NFC discovery and ISO14443-3A identification
- [ ] LF RFID read
- [ ] iButton read
- [ ] Sub-GHz receive: RSSI, then one decoded format
- [ ] Scoped file storage transfers

## Later

- [ ] Pi-side workflow layer: multi-step physical workflows with
      journaling, capture storage, and agent-facing tools
- [ ] On-device status UI: truthful link/job pages and local stop
- [ ] Backpack (Pi Zero 2 W) bring-up as the always-on rig

## Optional, individually gated

- [ ] IR raw capture/replay objects
- [ ] NFC / LF RFID / iButton emulation — one combination at a time,
      each proven against an independent reader and each behind
      explicit human confirmation
- [ ] Tag writing — a separate feature, deliberately not implied by
      read/emulate support
