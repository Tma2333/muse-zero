# Pi scripts

Scripts that run **on the Raspberry Pi** to drive the rig: wiring
self-checks, UART sniffers, IR-receiver fixtures, data loggers, and
other bench tooling around the bridge (the bridge's own test harness
lives with the app in `bridge/flipper-app/tools/`).

Nothing here yet. Conventions when scripts land:

- Python 3, standard library + `pyserial` unless noted; each script
  documents its wiring before its usage.
- Read-only by default: a script that transmits, writes files on the
  Flipper, or drives an output says so in its first paragraph.
- Fixtures describe exactly what physical proof they provide
  (e.g. "this sees any 38 kHz IR burst and timestamps it").
