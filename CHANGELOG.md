# Changelog

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
- Protocol documented in `docs/protocol.md`

Not in this build (by design): any transmit/emulation feature, tag
writing. Roadmap lives in the README.
