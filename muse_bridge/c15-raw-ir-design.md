# C15 (M6) — Raw IR and bounded object transfer: design

Status: DESIGN for review, 2026-10-08. All wire schemas are the
plan's fixed §22.3/22.4 assignments — nothing here invents opcodes
or payload meanings. Firmware facts were read from the local 1.4.3
clone (`lib/infrared/worker/infrared_worker.c/.h`).

## Operations (caps-advertised only after qualification)

| Op | Kind | Payload | Limit in caps |
| --- | --- | --- | --- |
| `IR_RX_RAW_START` 0x0410 | action | `timeout_ms:u32` (1–60000) | 60000 |
| `IR_TX_RAW` 0x0411 | action | `object_id:u64, frame_count:u8, timeout_ms:u32` (frame_count=1 only) | 60000 |
| `OBJECT_BEGIN` 0x0901 | action | see below | 512 |
| `OBJECT_CHUNK` 0x0902 | action | see below | 64 |
| `OBJECT_COMMIT` 0x0903 | action | `object_id:u64` | 0 |
| `OBJECT_READ` 0x0904 | query (cached, no hardware) | `object_id:u64, offset_timings:u16, count:u8` (count ≤ 32) | 32 |
| `OBJECT_RELEASE` 0x0905 | action | `object_id:u64` | 0 |

## Object store (new portable unit: `modules/module_object.c`)

Fixed allocation, no heap growth after init: one **mutable** slot
and one **committed** slot, each 512 × `u32` durations + metadata
(id, owner session, state, declared count, received count, CRC,
carrier fields, completeness, pin count, begin tick).

- `OBJECT_BEGIN` `{object_type:u8=1, timing_count:u16 (1–512),
  carrier_hz:u32, duty_permille:u16, starts_with_mark:u8=1,
  checksum:u32}` (14 bytes packed) — object_id = this action's sequence. Replay
  parameters must be exactly 38000 Hz / 330 permille in this
  increment; anything else is INVALID_ARGUMENT (they are explicit
  replay settings, not measurements). Mutable slot busy (another
  upload or capture owns it) → BUSY. Absolute upload lifetime 60 s
  from BEGIN; CHUNK/COMMIT after expiry find no object (purged).
- `OBJECT_CHUNK` `{object_id:u64, offset_timings:u16, count:u8
  (1–64), duration_us[count]:u32}` — writes must be the next
  contiguous offset; an already-written range with **identical**
  bytes is acknowledged without rewriting (this is what makes a
  retried chunk after a dropped reply safe at any layer); a
  conflicting or partially overlapping range is INVALID_ARGUMENT;
  holes can never commit. Duration *values* are validated at
  COMMIT, not per chunk (schema order per §22.4).
- `OBJECT_COMMIT` `{object_id:u64}` — requires received == declared;
  verifies CRC-32/ISO-HDLC over the little-endian durations (same
  CRC as the wire codec), each duration 1–1,000,000 µs, total
  ≤ 2,000,000 µs (64-bit sum). Success → immutable committed object
  (replacing any previous committed object), mutable slot freed.
  **No transmission occurs at commit.**
- `OBJECT_READ` — controller-served from copied data, never waits
  on the executor. Reply: `{present:u8, complete:u8,
  total_count:u16, returned_count:u8, crc32:u32, carrier_hz:u32,
  duty_permille:u16, carrier_measured:u8, starts_with_mark:u8,
  duration_us[returned]:u32}` ≤ 32 timings (176 B ceiling).
  Session envelope required (objects are session-owned; this is
  not a recovery query).
- `OBJECT_RELEASE` — frees committed or in-progress object; an
  object pinned by a running TX is never freed (pin checked; with
  the single executor this surfaces as BUSY only in the session-
  purge race, handled by the pin counter).
- Ownership: objects belong to the session that created them.
  Lease expiry alone keeps them (the identity is unchanged until a
  new handshake); a **replacing** session purges them — and a
  resume mints a fresh identity in this implementation, so a
  resume replaces and purges. App exit frees everything (RAM-only,
  like the ledger). Reconnect never recreates released objects.

## Raw RX (module_ir extension, op 0x0410)

Executor job, worker with decoding **disabled**
(`infrared_worker_rx_enable_signal_decoding(w, false)` — with
decoding on, decoded signals carry no raw timings). First received
burst ends the capture phase:

- count ≤ 512 → validate durations (1–1,000,000 µs each, total
  ≤ 2,000,000 µs) → publish committed object, object_id = action
  sequence, `capture_complete=true`, `carrier_measured=false`,
  carrier/duty recorded as 0 (unknown — never the replay defaults).
  Terminal OK; summary `{status, object_id, timing_count, crc32,
  capture_complete=1}`.
- count 513–1024 (complete worker capture over our cap) → publish
  as **incomplete diagnostic** (complete=false), terminal status
  **OVERFLOW (17)**. Never TX-able.
- Worker-level overrun (>1024 timings) is discarded by the worker
  itself (no callback, by SDK design) — the job sees silence and
  ends at timeout with no object. No truncated data can leak.
- Timeout with no burst → COMPLETE OK, no object (mirrors decoded
  RX's zero-event completions).
- Mutable slot owned by an upload → BUSY (single-slot rule).

Worker facts relied on: captures start on a Mark (leading Space is
skipped by the SDK), `MAX_TIMINGS_AMOUNT` = 1024 ≥ our 512 cap,
first-burst semantics identical to the decoded path's callback.

## Raw TX (module_ir extension, op 0x0411)

Validation: object exists, is committed **and complete**, owned by
this session, frame_count == 1, timeout 1–60000 ms. A partial or
incomplete object is refused before any hardware call — *partial
uploads never emit* is a validator property, not a runtime hope.

Provider: finite, mirroring C13 — first get-signal call copies the
object out via `infrared_worker_set_raw_signal(timings, count,
38000, 0.330f)` and returns New; every later call returns Stop.
Completion model is C13's, for a read-from-source reason: the
worker's TX thread seeds `repeats_left = 1` for raw signals and
decrements it for the first message, so the message-sent callback
is unreachable for a lone train exactly as with NEC. Job completes
on provider exhaustion; physical completion is established in
cleanup (tx_stop waits termination and joins). Summary
`{status, supplied=1, sent}` upgrades sent only on the exhaustion
path, as in C13.

SDK check (§10.5): `infrared_worker_set_raw_signal` prepends one
leading delay timing (`timings[0] = INFRARED_RAW_TX_TIMING_DELAY_US`),
so the worker sees count+1 ≤ 1024 — our 512 cap is safely inside;
this is accounted, not discovered later.

## Statuses

No new codes: refusals reuse INVALID_ARGUMENT / BUSY /
RESOURCE_UNAVAILABLE; capture over-cap uses the existing
**OVERFLOW (17)**. GET_CAPABILITIES gains the 7 rows above;
protocol 1.0 unchanged; app minor 10 → 11 (dev build
`mb-0.12-c15-1`). Release config unaffected (these are real
features; FAKE_RUN stays dev-only).

## Native tests (test_c15, host, ASan/UBSan)

Object store: begin validation (count 0/513, wrong carrier/duty,
type != 1); chunk contiguity; identical-range re-ack leaves bytes
unchanged (CRC invariant); conflicting overlap rejected; commit
with hole rejected; CRC mismatch rejected; duration 0 and
1,000,001 rejected at commit; total > 2,000,000 rejected; expiry
(60 s) purges; release frees; second BEGIN while mutable owned →
BUSY; session purge drops owned objects; read paging returns
≤ 32 with correct totals/CRC; incomplete (RX-overflow) object is
refused by the TX validator; pin prevents release during TX.

## Device qualification (harness case C15, gated bench run)

1. **Known waveform round-trip**: host builds NEC-shaped raw
   timings for addr 0x04/cmd 0x08 (~68 timings), BEGIN/CHUNK×2/
   COMMIT (count + CRC asserted at each step), OBJECT_READ pages
   match byte-for-byte, `IR_TX_RAW` → the Pi witness must decode
   exactly **one NEC data frame addr 4 / cmd 8**, no repeats.
2. **Raw capture**: owner fires the TV remote once → RX_RAW job →
   OBJECT_READ: count in a sane NEC range, host-side NEC timing
   decode of the returned durations = addr 0x04 / cmd 0x08,
   `carrier_measured=0`, CRC self-consistent.
3. **Partial upload never emits**: BEGIN + half the chunks →
   `IR_TX_RAW` refused; COMMIT with a hole refused; witness
   silent throughout.
4. **Over-cap**: BEGIN count 513 → INVALID_ARGUMENT. (RX over-cap
   stimulus — a >512-timing single burst — needs the Pi-side IR
   emitter module scripted as a signal source; if that fixture
   isn't ready, the RX-overflow row is BLOCKED_FIXTURE, not
   skipped silently.)
5. **Chunk reply drop**: chunk action's reply discarded by the
   harness; retry same sequence (ledger replay) and a fresh
   identical-range chunk action both leave CRC/commit unaffected.
6. **Disconnect mid-upload**: lease expiry (echo pause > 3 s) →
   resume same session → upload completes, object readable,
   TX works. Session replacement (different client) → old object
   reads absent.
7. Regressions: C04, C07, C12X, C13 on the new build; 100-cycle
   spot check (10 raw TX of the committed object, witness counts
   10) before PASS_DEVICE.

## Out of scope for C15

frame_count > 1, non-default carrier/duty, raw replay of captures
in the same job (capture → release/read → TX is host-orchestrated),
any other object type, storage-backed objects (C20).
