# Muse Zero

**Give an AI assistant hands that can touch the physical world — safely,
provably, one verified capability at a time.**

## What is Muse Zero?

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

## What can it do?

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

## Safety by design

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

## Status

Foundation released and verified on real hardware; expansion modules
(NFC, RFID, iButton, Sub-GHz) in progress, one at a time. Details and
direction in [ROADMAP.md](ROADMAP.md) and [CHANGELOG.md](CHANGELOG.md).

## License

MIT. See [LICENSE](LICENSE).
