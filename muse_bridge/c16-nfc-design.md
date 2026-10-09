# C16 design — NFC discovery + ISO14443-3A identify (M7)

Status: DRAFT for owner review (2026-10-08). Plan refs: §10.6
(steps A/B only), §22.3 opcode table, §22.4 minimum result
records, C16/M7 row (§21): "Implement scanner first, then
stop/free and use an ISO14443-3A poller. Test known card, no
card, removal, unsupported protocol, cancel, expiry, and 100
transitions." References R09 (stock detect scene) / R10 (stock
CLI Type-A dump). Fixture: SunFounder S50 card (MIFARE Classic
1K). Judge: the stock NFC app / phone NFC Tools read of the same
card, taken before the bridge runs.

## Scope

Two ACTION ops (hardware-touching reads use the ledger, §22.3):

| Op | Name | Request payload | Promise |
| --- | --- | --- | --- |
| 0x0501 | NFC_SCAN | `timeout_ms:u32` | protocol candidates only — NOT a card read |
| 0x0502 | NFC_IDENTIFY | `protocol:u16, timeout_ms:u32` | UID + ATQA + SAK for one supported protocol |

Protocol IDs: ISO14443-3A = 1 (§22.3). Everything else in §10.6
(step C read, step D emulate) is out of C16: no sector reads, no
authentication, no listener, no tag writing.

Timeout range 1–60000 ms for both ops (house rule, matching the
IR jobs). Payload lengths exact (4 / 6 bytes); violations are
consumed INVALID_ARGUMENT at validation, before any allocation
(plan §10.6 step 1: "Validate the explicitly supported protocol
before allocation" — protocol != 1 → consumed INVALID_ARGUMENT).

## Result records (§22.4 shapes, house status prefix as in C13/C15)

NFC_SCAN retained COMPLETE summary:

```
status:u16, mapped_count:u8, mapped[mapped_count]:u16, unmapped_count:u8
```

- The scanner reports raw `NfcProtocol` candidates. The bridge
  maps a candidate to a bridge protocol ID when the candidate
  IS that protocol or descends from it
  (`nfc_protocol_has_parent`): initially only Type-A (1) is
  mapped, so a MIFARE Classic detection (parent Iso14443_3a)
  maps to 1. The mapped list is deduplicated, in candidate
  order, capped at 4 entries.
- Candidates mapping to nothing increment `unmapped_count`
  (saturating u8). Unmapped candidates are informational only —
  they can never become an IDENTIFY parameter (§22.4).
- A full window with no detection completes OK with
  mapped_count = 0, unmapped_count = 0 (the listening job did
  its job; same convention as C15's empty capture).

NFC_IDENTIFY retained COMPLETE summary (fixed 21 bytes):

```
status:u16, found:u8, protocol:u16, uid_len:u8, uid[10]:u8,
atqa[2]:u8, sak:u8, error_events:u16
```

- found = 1: `protocol` echoes the requested (mapped) ID;
  `uid_len` in {4, 7, 10} (§22.4 — anything else is discarded
  and treated as not found); `uid` zero-padded past uid_len;
  `atqa` in SDK byte order (the Iso14443_3aData field order,
  documented with the golden vector — never reinterpreted as a
  swapped u16); `sak` raw.
- found = 0 (timeout, or only error events): all data fields
  zero. No stale data can survive into a later result — the
  record is built fresh per job (plan's removal-test intent).
- `error_events` counts poller Error events seen during the
  window (diagnostic; errors are non-fatal, polling continues —
  that is the stock poller's behavior, cf. R10).

## Platform shape (verified against the 1.4.3 SDK headers)

- One `Nfc*` per job: `nfc_alloc()` at job start,
  `nfc_free()` in cleanup. Either a scanner OR a poller on it,
  never both (plan §10.6); each op allocates its own, so the
  scanner→poller sequence the plan describes happens across two
  jobs, each fully stopped/freed before the next allocates.
- SCAN: `nfc_scanner_alloc(nfc)` → `nfc_scanner_start(scan, cb,
  ctx)`. The callback receives `NfcScannerEvent` by value with
  `protocol_num` + a borrowed `NfcProtocol*` array: copy the
  candidates into job-owned storage inside the callback, then
  end the job (first detection wins — consistent with the
  bridge's first-burst IR convention and the operator signal
  semantics: the end beep means "got it"). Cleanup:
  `nfc_scanner_stop` → `nfc_scanner_free` → `nfc_free`, all on
  the executor thread, never in the callback (R09).
- IDENTIFY: `nfc_poller_alloc(nfc, NfcProtocolIso14443_3a)` →
  `nfc_poller_start(poller, cb, ctx)`. The generic callback
  casts `event_data` to `Iso14443_3aPollerEvent*` (R10's actual
  handler pattern). On Ready: copy UID/ATQA/SAK out of
  `nfc_poller_get_data()` (public accessor; cast to
  `const Iso14443_3aData*`) into job-owned storage INSIDE the
  callback, return `NfcCommandStop`. On Error: count it, return
  `NfcCommandContinue`. No join/free from the callback.
  Cleanup: `nfc_poller_stop` → `nfc_poller_free` → `nfc_free`
  on the executor thread.
- The module runs the same executor lifecycle as IR: start()
  allocates and arms; wait() blocks on a job event flag with
  the deadline; the completion hook publishes the retained
  summary. Cancel/timeout/lease-loss all funnel through stop()
  → cleanup frees everything.

## Bench signals (v2 language, operator revision 2026-10-08)

Both ops are listening jobs, and they implement the current
signal language (protocol.md will be updated to match at
close): silent start; blue LED flashing while listening; a
beep at the FIRST intended signal received (scan: first
detection; identify: card Ready) and only then; clean close —
including an empty window — silent; error, overflow, or
forced exit (cancel/lease loss) → red LED flashes during
cleanup. SIGNALS_OFF mutes all of it (the 100-cycle case sends
it first). The IR glue migrates to the same shared helpers
(`bridge_listen_begin/end`, a received-hook, an error-exit
hook); IR behavior changes to v2 with this build, so
C12X/C13/C15 spot rows join the regression set.

## Native gate (tests/native/test_c16.c)

Pure logic + a scripted fake NFC backend, same pattern as
test_c15r:

- Candidate mapper: dedupe, parent mapping (MfClassic/
  MfUltralight/Iso14443_4a → 1), unmapped counting (Felica,
  Iso15693_3 → unmapped), cap behavior, empty list.
- Identify record builder: uid_len 4/7/10 accepted verbatim;
  0/5/11 rejected to not-found; ATQA byte order preserved;
  zero-fill on not-found.
- Request validators: payload sizes, timeout range,
  protocol == 1 only.
- Job state machine against the fake backend: alloc failure →
  RESOURCE_UNAVAILABLE; no card → timeout found=0; Ready →
  record copied (mutating the fake's borrowed buffer after the
  callback must not change the retained record — the
  no-borrowed-pointer proof); Error-then-Ready; cancel
  mid-window; 100 start/stop cycles with balanced alloc/free
  counters.

## Device qualification matrix (harness case C16; bench-gated)

Fixture values established first: stock NFC app (or phone) read
of the S50 → expected UID / ATQA 04 00 / SAK 08 recorded.

1. Caps: 0x0501/0x0502 rows present, limits 60000; build
   string; C04 regression 8/8.
2. SCAN, no card in range: OK, mapped 0, unmapped 0.
3. SCAN, S50 presented on the signal: OK, mapped = [1],
   unmapped = 0.
4. IDENTIFY protocol 2 and protocol 0 → consumed
   INVALID_ARGUMENT; wrong payload sizes → consumed
   INVALID_ARGUMENT; nothing allocated (a following valid
   IDENTIFY works immediately).
5. IDENTIFY, S50 on the signal: found=1, protocol=1,
   uid_len=4, UID byte-equal to the fixture read, ATQA =
   [04 00], SAK = 08.
6. IDENTIFY, no card: found=0, all-zero record, completes at
   the window edge.
7. Removal: after row 5, card removed → IDENTIFY found=0 and
   SCAN mapped 0 (no retained data anywhere).
8. Cancel: 30 s SCAN with no card, cancel at ~2 s → CANCELLED
   terminal; NFC immediately reusable (row 5 repeats OK).
9. Expiry: lease killed mid-SCAN → job fenced at expiry; after
   resume, IDENTIFY works.
10. Cycles (case C16T): SIGNALS_OFF, card parked on the
    reader, 100 × IDENTIFY — all found=1 with the same UID;
    heap free/largest-block in result.txt within noise of the
    C15 baseline.
11. Regressions: C12X (decoded RX) and C13 (2-frame witnessed
    TX) re-run on the new build — the listen-signal refactor
    touched IR glue.

## Firmware integration checklist

- `modules/module_nfc.{c,h}` (executor module + native core),
  `application.fam` source list extended; caps gains exactly 2
  rows; GET_INFO app minor → 12; dev build id
  `mb-0.13-c16-1`; result.txt counters `nfc_scan`, `nfc_id`,
  `nfc_err`.
- First-build link check: lib/nfc symbols (scanner, poller,
  protocol helpers) resolve under ufbt API 87.1 for an external
  FAP; if any symbol is missing from the SDK's exported set,
  stop and report — no workarounds that reach into firmware
  internals.
- protocol.md gains the C16 section at close; evidence file
  `evidence/C16-nfc-2026-10-xx.md`.

## Open questions for the owner

None on scope — §10.6/§22 fix it. One judgment call to confirm:
first-detection-wins for SCAN (fast acknowledgment beep, one
candidate snapshot) vs run-the-full-window (greedy complete
list). This design takes first-detection; the candidate list
from one scanner event already enumerates everything the
scanner found in that pass.
