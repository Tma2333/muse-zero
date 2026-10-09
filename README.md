# Muse Zero

**Give an AI assistant hands that can touch the physical world — safely,
provably, one verified capability at a time.**

## Introduction

### What is Muse Zero?

Muse Zero connects Muse to a Flipper Zero through a Raspberry Pi
(Flipper Zero + Pi Zero + Muse). Muse lives on the Pi; the Pi talks to
the Flipper over three wires; and the Flipper becomes something Muse can
*sense and act through* — read pins and voltages, hear an infrared
remote, flash a light, feel a tag — without giving up the rigor of
software: sessions, leases, cancellation, and an audit trail for
everything that happened.

The project's center is the **Muse Bridge**, a Flipper app that exposes
the Flipper's hardware through a protocol designed like an API you'd
trust: one owner at a time, leases that expire when the link dies,
exactly-once execution (a repeated request replays its result instead
of running twice), and hard stops for anything that transmits.

### What can it do?

Today, on stock Flipper firmware:

- **Sense** — read GPIO pins and analog voltages, receive and decode
  infrared remote signals
- **Signal** — short, finite notifications: an LED flash, a beep, a buzz
- **Stay safe doing it** — every hardware claim is verified against a
  physical observer before it joins the feature list

Coming, each as its own verified module: NFC and RFID reading, iButton,
Sub-GHz listening, and the Pi-side workflow layer that lets Muse run
multi-step physical procedures.

Plus a lighter side: the **Jolly animations**, 33 desktop animations
that make any Flipper a little more alive — no bridge required.

### Safety by design

- **Leases, not trust** — if the link dies, control expires and any
  running job stops; nothing keeps acting on stale instructions.
- **Exactly once** — retries and duplicates are safe; hardware actions
  don't double-execute.
- **Outputs are earned** — transmit and emulation features are absent
  until each is proven against an independent receiver and approved by
  a human, one at a time.

## Projects

| Section | What it is |
|---|---|
| [`muse_bridge/`](muse_bridge/) | The center: Muse Bridge app for the Flipper, its Pi-side half, and the wire protocol between them. |
| [`jolly_animations/`](jolly_animations/) | Drop-in Flipper desktop animations (the Jolly pack) with previews. |

## Getting started

- **Just want the animations?** Copy them onto your Flipper —
  [jolly_animations/SETUP.md](jolly_animations/SETUP.md).
- **Building the full rig?** [SETUP.md](SETUP.md) walks the general
  Muse ↔ Flipper link (pairing, wiring, stock RPC), step by step,
  written so an AI assistant can follow it. The bridge app has its own
  guide in [muse_bridge/SETUP.md](muse_bridge/SETUP.md).

## Roadmap

High-level only; every item lands as a module behind the session/
executor foundation, verified on real hardware before it joins the
advertised capability list. Release history: [CHANGELOG.md](CHANGELOG.md).

**Done**

- [x] Session foundation: handshake, expiring leases, one owner at a
      time, same-client resume
- [x] Exactly-once actions with cancellation, deadlines, and
      post-restart outcome lookup
- [x] Diagnostic trace readable over the wire
- [x] GPIO: configure/read/write/release with auto-release on link loss
- [x] Analog voltage reads (multi-sample averaged)
- [x] Finite notifications: LED flash, beep, vibration
- [x] Infrared receive: decoded signals with sequenced event streams
- [x] Infrared transmit: one finite decoded frame per action,
      exactly-once proven by an independent bench receiver
- [x] **P1-A release qualification (bridge 1.0)**: full link
      fault-injection matrix, 100-cycle endurance per module,
      30-minute receive soak with injected link drops, release build
      with the development test operation removed
- [x] Raw IR: capture a real remote's timing waveform into a
      bounded object, replay it as one finite train — captured and
      uploaded waveforms both proven against the bench receiver
- [x] Bench signals: the device announces listening state itself
      (double beep at open/close, flashing LED while listening), so
      a human at the bench never has to guess test timing

**Next**

- [ ] NFC discovery and card identification
- [ ] LF RFID read
- [ ] Sub-GHz receive: signal strength, then one decoded format
- [ ] Scoped file storage transfers

**Later**

- [ ] Pi-side workflow layer: multi-step physical workflows with
      journaling and agent-facing tools
- [ ] On-device status UI: truthful link/job pages, local stop
- [ ] Backpack (Pi Zero 2 W) bring-up as the always-on rig

**Optional, individually gated**

- [ ] iButton read
- [ ] NFC / LF RFID / iButton emulation — one combination at a time,
      each proven against an independent reader and each behind
      explicit human confirmation
- [ ] Tag writing — a separate feature, deliberately not implied by
      read/emulate support

## License

MIT. See [LICENSE](LICENSE).
