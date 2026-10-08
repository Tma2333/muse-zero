# Muse Bridge — P1-A release notes (1.0)

**Artifact:** `dist/release/muse_bridge.fap`
**Build ID:** `mb-1.0-rel-1` (GET_INFO app version 1.0, protocol 1.0)
**SHA-256:** `1d0d3f5e8625850657560d45296adabd0e6bb0322bd9c578563514ecfc19a779`
**Size:** 42,860 bytes
**Target:** Flipper Zero official firmware **1.4.3** (SDK API 87.1, target 7).
No unresolved imports (APPCHK clean). The dev build (`mb-0.11-c13-2`)
is a separate artifact; the release build differs only by
configuration: the dev-only FAKE_RUN operation is compiled out of
dispatch and out of the capability table, and the 90 s auto-exit
safety timer is removed (local Back exit is the release lifecycle).

## Qualified operations (exactly what GET_CAPABILITIES advertises)

Queries: PING `0x0001`, GET_INFO `0x0002`, GET_CAPABILITIES `0x0003`,
GET_RESULT `0x0005`, CANCEL `0x0006`, GET_TRACE `0x0008`.

Actions (all exactly-once under the session/lease/ledger model):

| Op | Operation | Limits |
| --- | --- | --- |
| `0x0101`–`0x0104` | GPIO config / read / write / release | Header pins **2–7** only; hold ≤ 600 s |
| `0x0201` | ADC read | 1–16 samples, pins pre-qualified per C11 map |
| `0x0301` | Notify | 3 finite effects (green flash / short beep / short vibe), ≤ 250 ms |
| `0x0401` | IR receive, decoded | NEC only, 1–60 s jobs, per-event sequence numbers |
| `0x0402` | IR transmit, decoded | NEC only, exactly **one** frame, address/command ≤ 0xFF, repeat=false |

## Not in 1.0 (deliberately)

- FAKE_RUN (development/test operation — refused UNSUPPORTED and not
  advertised by the release build)
- GET_STATUS / CLOSE_SESSION (never implemented; not advertised)
- Raw IR capture/replay (C15), NFC (C16), LF RFID (C17), iButton
  (C18, optional by owner ruling), Sub-GHz (C19), scoped storage
  (C20), any emulation or tag writing (C21) — all later increments,
  each gated on its own fixture + qualification
- Sub-GHz transmit of any kind

## Known limitations

- One client session at a time; a competing client is refused BUSY.
- The FAP must be in the foreground and the Flipper **unlocked**; it
  cannot start or run while the device is locked.
- Sessions and the dedup ledger are RAM-only: if the FAP restarts,
  unresolved actions are reported INDETERMINATE_AFTER_RESTART and are
  never silently replayed.
- IR TX completion rests on provider exhaustion plus the quiescing
  TX stop/join in cleanup (firmware 1.4.3's worker never raises its
  message-sent callback for a lone frame). Physical proof for TX
  qualification came from an independent receiver, not from counters.
- Link: UART over header pins 13/14 at 230400 baud; expect the
  documented rx_error counts under load (benign, characterized).

## Qualification summary (2026-10-04 → 2026-10-08)

- Checkpoints C00–C13 all PASS_DEVICE (see `evidence/`).
- Host-native suite: all suites green under ASan/UBSan
  (`tests/native/run_all.sh`).
- Link fault cases L01–L25: device evidence per
  `evidence/C14-link-coverage-2026-10-08.md`.
- Lifecycle: 100 start/stop cycles per module (GPIO 200/200 actions,
  ADC / NOTIFY / IR RX 100/100 each) with **zero largest-block heap
  decline** per launch (result.txt records in the C14 evidence).
- Soak: 30 min continuous IR receive with injected link expiries on
  this release binary — results in the C14 evidence file.
- Release-binary smoke: handshake, capability truth (14 ops, no
  FAKE_RUN), FAKE_RUN refused, pin rejection (99 and 8), finite IR,
  disconnect/reconnect, local exit — C14 evidence file.

## Fixtures used for qualification

A TV remote (NEC addr 0x04 / cmd 0x08) as the known IR source;
HiLetgo HW-490 (VS1838) receiver on the Pi (GPIO27, internal pull-up)
as the independent IR witness; Pi GPIO17 observation line for GPIO
proofs; the bench lamp/remote and card fixtures belong to later
increments.
