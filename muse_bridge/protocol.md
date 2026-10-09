# Muse Bridge wire protocol (implemented subset: C04–C13)

Status: implemented through checkpoint C04 (bootstrap only). Sessions,
the action ledger, and all hardware operations arrive in C05+; nothing
below implies they exist yet.

## Transport

- Raw USART on Flipper pins 13 (TX) / 14 (RX), 230400 8N1, owned by the
  `muse_bridge` FAP (expansion service disabled while running).
- Framing: COBS-encoded body terminated by a single 0x00 delimiter.
  Decoded frame ≤ 512 B; payload ≤ 384 B. Partial frames older than
  250 ms are discarded through the next delimiter (never spliced).

## Envelope (60-byte header, little-endian; plan §6.5)

| Offset | Field |
| --- | --- |
| 0 | magic 0x42 0x52 ("BR") |
| 2 | protocol major (1), minor (0) |
| 4 | type |
| 5 | flags (0) |
| 6 | opcode u16 |
| 8 | status u16 |
| 10 | payload length u16 |
| 12 | connection generation u32 |
| 16 | correlation u64 |
| 24 | event_seq u32 |
| 28 | boot_id 16 B |
| 44 | session_id 16 B |
| 60 | payload, then CRC-32/ISO-HDLC u32 over all preceding bytes |

Frame types: 1 HELLO, 2 HELLO_REPLY, 3 CONFIRM, 4 SESSION_READY,
5 HEARTBEAT_CHALLENGE, 6 HEARTBEAT_ECHO, 7 QUERY, 8 REQUEST, 9 RESULT,
10 ACCEPTED, 11 EVENT, 12 COMPLETE.

Integrity: CRC-32/ISO-HDLC (check value for "123456789" = 0xCBF43926).
Any frame failing COBS/length/magic/version/CRC validation is counted
and dropped; it can never dispatch.

## Bootstrap operations (C04)

Operational queries require a session identity from C06 on. Bootstrap
queries carry zero boot/session IDs and generation 0 and have no
hardware effects:

- `PING` (QUERY, op 0x0001): request payload = `len:u8 + bytes`
  (len ≤ 32). RESULT payload = `len:u8 + bytes + uptime_ticks:u32 +
  tick_frequency:u32`.
- `GET_INFO` (QUERY, op 0x0002): request payload = `page:u8` (only page
  0 exists). RESULT payload = `page_type:u8(0), next_cursor:u16(0xFFFF
  = none), item_count:u8, proto_major:u8, proto_minor:u8, app_major:u8,
  app_minor:u8, fw_api_major:u16, fw_api_minor:u16,
  tick_frequency:u32, build_len:u8 + build text`. The reply envelope
  carries the FAP's random-per-launch boot_id.
- Any other QUERY op, and any REQUEST, gets a RESULT with status
  UNSUPPORTED (never silence, never a crash). Actions stay disabled
  until C06/C07 pass.

## Session operations (C06)

Implemented in `core/bridge_session.*` on the section-21 reference lease
(`core/bridge_core.h`). Payloads are little-endian; `id128` is 16 bytes.

- `HELLO` (type 1): payload = `client_instance_id:id128,
  handshake_nonce:id128, prior_boot_id:id128, prior_session_id:id128`
  (64 B). Envelope identity must be zero. Fresh handshakes use zero
  priors; a resume names the retained boot/session.
- `HELLO_REPLY` (type 2): envelope identity = the proposal. On success:
  status OK + 69 B payload `client_instance_id, handshake_nonce,
  challenge:u64, last_consumed_seq:u64, active_job_id:u64,
  challenge_interval_ms:u16(500), proof_max_age_ms:u16(1000),
  suspect_ms:u16(1500), lease_ms:u16(3000), max_payload:u16(176),
  ledger_entries:u8(16), link_state:u8, executor_state:u8`. On error
  (BUSY / TRY_LATER_NOT_ADMITTED / INVALID_FRAME): empty payload — an
  error status carries no proposal. One pending proposal exists at a
  time (2000 ms TTL); a repeated HELLO with the same client+nonce
  returns it unchanged, and a repeated HELLO for a completed handshake
  returns the completed transaction so the client can recover a lost
  reply by re-CONFIRMing.
- `CONFIRM` (type 3): payload = `client_instance_id, handshake_nonce,
  challenge:u64` (40 B); the envelope identity must equal the proposal.
  Success: `SESSION_READY` (type 4), envelope = committed identity,
  payload = `handshake_nonce, last_consumed_seq:u64, active_job_id:u64,
  link_state:u8, executor_state:u8` (34 B). Exactly one commit per fresh
  handshake; an exact duplicate CONFIRM returns the cached READY
  without re-committing or renewing the lease. Failures return `RESULT`
  (type 9, op 3) with MB_NO_SESSION (no/superseded handshake),
  MB_STALE_CONNECTION (envelope mismatch), MB_INVALID_ARGUMENT (wrong
  challenge), MB_TIMEOUT (proposal older than 2000 ms or challenge
  older than proof_max_age), or MB_TRY_LATER_NOT_ADMITTED (previous
  owner's cleanup incomplete).
- `HEARTBEAT_CHALLENGE` (type 5, FAP→Pi) / `HEARTBEAT_ECHO` (type 6):
  payload = `challenge_counter:u64`; envelope = committed identity.
  While linked the FAP offers a fresh challenge every 500 ms. Only a
  fresh echo (current generation, counter never accepted before, round
  trip within proof_max_age) renews the lease. Link states: 0 waiting,
  1 live, 2 suspect (proof stale ≥1500 ms), 3 expired (≥3000 ms,
  sticky). A different client gets BUSY while an owner is live/suspect;
  it may take over after expiry once cleanup is done. A replaced
  session's identity is never resurrected; the replacing owner's
  session is the retained one (resume rotates its generation).

## Action admission, replay, recovery (C07)

- A `REQUEST` (type 8) carries: `boot_id`, `session_id`, `generation` = the
  retained session; `event_seq` = 0; `correlation` = the client's
  monotonically increasing **request sequence**.
- Replies: `ACCEPTED` (10) when the worker starts; `EVENT` (11) per data
  record (same `correlation`, `event_seq` = record sequence from 1, payload
  ≤ 32 bytes); `COMPLETE` (12) exactly once, only after the outcome is
  durable in the ledger; `RESULT` (9) for synchronous refusals and replayed
  immediate outcomes.
- Admission first validates semantics (unknown op → `UNSUPPORTED`, bad
  payload → `INVALID_ARGUMENT`, executor busy → `BUSY`); these consumptions
  are ledgered. Envelope, session, liveness, and sequence failures
  (`INVALID_FRAME`, `NO_SESSION`, `STALE_CONNECTION`, `TRY_LATER`,
  `SEQUENCE_GAP`, `REQUEST_ID_REUSED`) never consume the sequence.
- While the link is SUSPECT, `REQUEST` → `TRY_LATER_NOT_ADMITTED` and
  consumes nothing; an identical retry after a fresh proof is admitted.
- Duplicate of an identical in-flight request → `ACCEPTED` again; duplicate
  of a completed one → `COMPLETE` with the retained summary; duplicate of a
  consumed rejection → `RESULT` with the recorded status. Duplicate with a
  different payload → `REQUEST_ID_REUSED`. Retention: 16 ledger records; an
  evicted sequence answers `STALE_REQUEST`.
- `QUERY GET_RESULT` (op 0x0005, payload = request sequence `u64`) returns
  `present:u8, entry_state:u8, status:u16, result_len:u16, result[]` and
  never renews the lease. Entry states: 0 empty, 1 accepted, 2 started,
  3 terminal.
- The only operation in the qualification build is `FAKE_RUN` (0x7F01),
  payload `duration_ms:u32, event_interval_ms:u16` (duration 0 defaults to
  10000 ms; > 60000 ms → `INVALID_ARGUMENT`; interval 0 disables events).
- On lease expiry the controller requests a stop with the link reason;
  ledger identity survives while the same session is retained.

## Cancel, deadlines, trace (C08)

- `CANCEL` (QUERY op 0x0006, payload = job id `u64`, i.e. the request
  sequence) requires the exact retained identity (boot, session,
  generation); a zero envelope answers `NO_SESSION`, alien ids answer
  `NO_SESSION`, a wrong generation answers `STALE_CONNECTION`. It
  consumes no action sequence. A running job answers `STOP_REQUESTED`
  and terminates via the normal `COMPLETE` with status `CANCELLED`;
  a terminal job answers with its recorded status (repeated
  cancellation returns the known state); an unknown job answers
  `OUTCOME_UNAVAILABLE` and never affects the current job. The stop
  latches only for the executor's current job id, so a stale cancel
  can never stop a newer job.
- Hardware stops at the earliest of job deadline, lease expiry,
  explicit cancel, or local Back; a heartbeat proof never extends the
  original deadline. On lease expiry the stop intent latches in the
  same controller poll (measured 0 ticks, gate ≤ 100 ms), and the job
  terminates with `LINK_LOST`. Local Back requests a stop
  (`MB_STOP_LOCAL` → `CANCELLED`) and the app exits once the slot is
  idle. `HW_QUIESCENT` is traced after cleanup with the outcome
  durable in the ledger.
- `GET_TRACE` (QUERY op 0x0008, payload `after_seq:u32, limit:u8`,
  limit clamped to 4) is open to the all-zero envelope or the exact
  retained identity and never renews the lease. The reply payload is
  `next_after_seq:u32, has_more:u8, oldest_seq:u32, newest_seq:u32,
  count:u8, gap:u8`, then count 32-byte records
  (`trace_seq:u32, tick:u32, job_id:u64, generation:u32, arg0:u32,
  arg1:u32, event_code:u16, session_epoch:u16`).

## Reconnect and restart recovery (C09)

- A 500 ms silence is invisible (job and session continue). At
  1500 ms without a fresh round-trip proof the link is SUSPECT:
  `REQUEST` answers `TRY_LATER_NOT_ADMITTED` and consumes nothing.
  At 3000 ms the lease expires: revocation latches, a running job
  stops (`LINK_LOST`), and replayed stale echoes never resurrect it.
- Resume presents the prior boot/session ids with a **fresh nonce**
  (the completed nonce returns the cached transaction instead). The
  ledger and its outcomes survive; the generation rotates by one.
  Admission stays refused until the controller has transferred the
  old revocation (executor idle). Actions framed with a dead
  generation are refused `STALE_CONNECTION`.
- A new owner receives `BUSY` while the old lease is live, and its
  `CONFIRM` is refused `TRY_LATER_NOT_ADMITTED` until the old cleanup
  completes; only a successful commit resets the ledger.
- A restarted FAP has a new `boot_id` and no retained session: the
  old identity's `GET_RESULT` answers `OUTCOME_UNAVAILABLE`, the old
  action is client-side `INDETERMINATE_AFTER_RESTART`, and it is
  never automatically resubmitted.
- `COMPLETE` summaries carry the generated/dropped record counts; a
  client reconciles received `event_seq` values against them and
  treats any hole as an incomplete capture, never as success.

## Golden vectors

Wire bytes (COBS, delimiter excluded) for boot_id[0]=1, session_id[0]=2,
generation=1. Generated independently by `tests/native/print_vectors.c`
(C) and `tools/bridge_codec.py --vectors` (Python); outputs are
byte-identical.

- V1 QUERY/PING corr=9 payload {2,'O','K'}:
  `04425201020702010101020302010101020901010101010101010101020101010101010101010101010101010202010101010101010101010101010108024f4b0677cdbe`
- V2 RESULT/PING corr=9 payload {3,'a','b','c',uptime=0x01020304,freq=1000}:
  `04425201020902010101020c0201010102090101010101010101010102010101010101010101010101010101020201010101010101010101010101010b0361626304030201e8030105fe6939ed`
- V3 QUERY/GET_INFO corr=10 payload {0}:
  `04425201020702020101020102010101020a0101010101010101010102010101010101010101010101010101020201010101010101010101010101010105f4c2e1f6`

## GET_CAPABILITIES (C10, op 0x0003)

QUERY, bootstrap or session envelope. Payload `cursor:u16` (LE);
RESULT payload: `next_cursor:u16, count:u8, reserved:u8` then count
records of `op:u16, kind:u8 (1=action), limit:u32`. Records in
ascending op order: PING 0x0001, GET_INFO 0x0002, GET_CAPABILITIES
0x0003, GET_RESULT 0x0005, CANCEL 0x0006, GET_TRACE 0x0008 (queries,
limit 0), GPIO_CONFIG 0x0101 (limit 60000 = max hold ms), GPIO_READ
0x0102, GPIO_WRITE 0x0103, GPIO_RELEASE 0x0104, ADC_READ 0x0201
(limit 16 = max samples), NOTIFY 0x0301 (limit 250 = max effect ms),
FAKE_RUN 0x7F01 (limit 60000 = max duration ms; qualification only).
GET_STATUS (0x0004) is not advertised: it is not implemented.

## GPIO (C10, ops 0x0101–0x0104)

REQUESTs on the session; exactly-once ledger semantics (C07). Pins
are header numbers 2–7 only; a claim persists past the instantaneous
job until RELEASE, hold expiry, lease loss, or app exit.

- GPIO_CONFIG `pin,mode,pull,initial_value,hold_ms:u32`: mode 0 =
  input, 1 = push-pull output. Output requires pull = none and a
  bounded hold (0 selects the 30000 ms default; max 60000, values
  above are rejected). Input requires pull ∈ {none, up, down} and
  initial_value = 0, hold_ms = 0. Result `status,pin,mode,pull`.
- GPIO_READ `pin`: configured inputs only.
  Result `status,pin,mode,pull,level`.
- GPIO_WRITE `pin,value` (0/1): configured outputs only; never
  refreshes the hold deadline. Result `status,pin,value`.
- GPIO_RELEASE `pin`: restores the analog/no-pull default.
  Result `status,pin`.

Refusals are consumed: invalid shape/pin/mode → INVALID_ARGUMENT;
a second claim or mode change on a claimed pin →
RESOURCE_UNAVAILABLE. All multi-byte fields are little-endian;
result payloads begin with `status:u16`.

## ADC (C11, op 0x0201)

REQUEST `pin:u8, samples:u8` (1–16); ADC-capable allowlisted pins
only (2→ch12, 3→ch11, 4→ch9, 7→ch4; pins 5 and 6 have no ADC). The
pin must be unclaimed (GPIO claim → RESOURCE_UNAVAILABLE) and is
never kept: reads run on the default 0–2048 mV scale and the pin
returns to analog default. Result
`status,pin,channel,samples,raw_mean:u16,millivolts_mean:u16`.
raw is the 12-bit code (0–4095); inputs above 2048 mV saturate
(raw 4095, millivolts 2048).

## Notifications (C11, op 0x0301)

REQUEST `effect:u8`: 1 = green flash, 2 = short beep,
3 = short vibration. Each effect is finite (≤ 250 ms; measured
~100 ms), runs on the executor, and cannot be parameterised: no
arbitrary sequence upload exists. Result `status,effect`.

## IR receive (C12, op 0x0401)

REQUEST `protocol_filter:u16, timeout_ms:u32` (1–60000). P1-A
qualifies bridge protocol id 1 (NEC) only; other filter values are
INVALID_ARGUMENT. The job runs the firmware IR worker for the
bounded window and completes OK at timeout (cancellation and link
expiry end it sooner with their own statuses).

EVENT frames during the job carry one decoded frame each:
`event_seq:u32, protocol:u8, repeat:u8, address:u32, command:u32`
(little-endian). event_seq is assigned at decode time, per job,
starting at 1, so received + holes == last_seq exactly. Values are
copied in the worker callback; only qualified-protocol frames are
counted. The retained COMPLETE summary is
`status:u16, decoded:u32, emitted:u32, dropped:u32` with
decoded == emitted + dropped always: drops are accounted, never
silent (decode ring full, pool full, or frames stranded at job end).

## IR transmit, decoded (C13, op 0x0402)

REQUEST `protocol:u16, address:u32, command:u32, frame_count:u8,
timeout_ms:u32` (15 bytes). Initial qualification is deliberately
narrow: protocol 1 (NEC), frame_count exactly 1, standard NEC field
widths (address and command each ≤ 0xFF), timeout 1–60000 ms. Every
other combination is a consumed INVALID_ARGUMENT and nothing reaches
the LED.

The job drives the firmware IR worker with a finite provider: the
frame is supplied exactly once (worker response New, repeat=false),
then the provider reports Stop on every subsequent call, including
worker prefetch — the steady-signal Same response is never used, so
the worker cannot loop the frame. Provider exhaustion, the worker's
message-sent callback, and executor stop/join are distinct states:
the job completes OK when the provider is exhausted (frame fully
encoded into the worker's stream), and the retained terminal is
recorded only after cleanup's TX stop has waited out any in-flight
signal and joined the worker (emitter quiescent). The platform's
message-sent callback never fires for a lone frame (the 1.4.3 worker
spends NEC's minimum repeat count of 1 on the first message, which
raises no sent event), so it is not the completion trigger; if the
worker never finishes asking, the job's own timeout ends it with
TIMEOUT instead of a fabricated success. The retained COMPLETE
summary is `status:u16, supplied:u32, sent:u32`: frames handed to
the worker, and frames confirmed physically complete — for a job
that finished via exhaustion, the quiescing join in cleanup confirms
every supplied frame; on timeout/stop paths the raw callback count
stands.

Exactly-once is end-to-end: resending an admitted action ID replays
the retained COMPLETE from the ledger and emits nothing; a genuinely
new action ID can transmit again after cleanup. Physical proof is a
separate channel: an independent receiver (Pi GPIO + VS1838,
`tools/witness_ir.py`) counts the frames that actually left the LED,
and the C13 gate compares that count against the admissions.

## Notifications — bench signals (v2, 2026-10-08)

Effects 4–8 (op 0x0301) generalize the notification module into the
bridge's device-signal surface, operator-designed: the hardware
itself announces state, so a human at the bench never has to infer
timing from a chat message. (v1 — double-beep at open and close —
shipped in the C15 build and was superseded the same day.)

- `DOUBLE_BEEP` (4): two 100 ms notes with a 50 ms gap. Explicit
  actions always play — the mute gate covers only AUTOMATIC
  annunciation.
- `LISTEN_ON` (5) / `LISTEN_OFF` (6): drive the listening
  indicator. While anything is listening (flag set), the main loop
  flashes the blue LED about once a second; the blink sequence
  extinguishes itself, so clearing the flag needs no LED cleanup.
- `SIGNALS_OFF` (7) / `SIGNALS_ON` (8): mute/unmute ALL automatic
  annunciation (long cycle tests run SIGNALS_OFF first).

Automatic signals (v2): listening starts SILENT — the flashing
blue LED is the only "I am listening" signal, like a webcam light.
A single short beep fires ONLY at the moment the intended signal
is received ("got it") — its only meaning. A clean close,
including an empty window, is silent. A listening job that ends
abnormally (error status, OVERFLOW, host cancel, lease loss)
flashes the red LED three times during exit. Received hooks:
IR decoded (first drained frame), IR raw (first within-cap
capture — over-cap never beeps), NFC (first candidate list /
first valid identify record). The flag is the truth about
listening; the mute gate silences annunciation, never the flag.

Harness note (learned the hard way at C16): the NOTIFY request
payload is exactly ONE byte (the effect). A longer payload is a
consumed INVALID_ARGUMENT and the cue silently never plays —
cases must verify their signal actions COMPLETE.

## Raw IR + bounded objects (C15, ops 0x0410/0x0411, 0x0901–0x0905)

Design: `docs/c15-raw-ir-design.md` (plan §22.3/22.4). One mutable
object slot and one committed slot, each a fixed 512×u32 buffer of
microsecond durations plus metadata — no allocation after boot.
Objects are session-owned; the object id is the creating action's
sequence. Objects survive lease expiry (the identity is unchanged)
but a replacing session — including a resume, which mints a fresh
identity — purges the previous owner's objects once the executor
is idle. App exit frees everything (RAM-only, like the ledger).

- `OBJECT_BEGIN` (action) `object_type:u8=1, timing_count:u16
  (1–512), carrier_hz:u32, duty_permille:u16, starts_with_mark:u8=1,
  checksum:u32` (14 bytes). Replay parameters must be exactly
  38000 Hz / 330 permille in this increment — they are explicit
  replay settings, never measurements; anything else is a consumed
  INVALID_ARGUMENT. A busy mutable slot is an admitted BUSY
  terminal. Uploads expire absolutely 60 s after BEGIN.
- `OBJECT_CHUNK` (action) `object_id:u64, offset:u16, count:u8
  (1–64), duration_us[count]:u32`. Chunks must be contiguous. An
  already-written identical range is acknowledged without
  rewriting (a lost reply is recoverable two ways: same-seq ledger
  replay, or a fresh identical chunk); conflicting or overlapping
  data is an admitted INVALID_ARGUMENT terminal; holes can never
  commit.
- `OBJECT_COMMIT` (action) `object_id:u64`. Verifies received ==
  declared count, each duration 1–1000000 µs, total ≤ 2000000 µs
  (64-bit accumulation), and CRC-32/ISO-HDLC over the canonical
  little-endian durations against the declared checksum. Only a
  successful commit produces a transmissible object. Summary:
  `status:u16, object_id:u64, crc32:u32` (BEGIN/CHUNK/RELEASE
  share this shape, crc 0).
- `OBJECT_READ` (query, session envelope required; a foreign
  identity gets OUTCOME_UNAVAILABLE) `object_id:u64, offset:u16,
  count:u8 (1–32)`. Reply: `present, capture_complete,
  from_capture, carrier_measured:u8, total_count:u16,
  returned:u8, crc32:u32, carrier_hz:u32, duty_permille:u16,
  starts_with_mark:u8, timings[returned]:u32`. An unknown or
  foreign id returns RESOURCE_UNAVAILABLE with no payload. A
  capture reports carrier 0 / measured 0 — unknown, never the
  replay defaults.
- `OBJECT_RELEASE` (action) `object_id:u64`. Frees a committed or
  in-progress object; an object pinned by a running TX is never
  freed (BUSY).
- `IR_RX_RAW_START` (action, op 0x0410) `timeout_ms:u32`
  (1–60000). The IR worker runs with decoding disabled; the first
  burst is staged (up to the worker's 1024-timing capacity) and
  ends the job. At terminal publication the capture is published
  into the committed slot: ≤512 valid timings → complete object,
  carrier unknown. A count of 513–1024 ends the job with OVERFLOW
  and publishes an explicitly incomplete diagnostic object (never
  transmissible); >1024 the worker itself discards the burst and
  the job times out with no object. Timeout with no burst is an OK
  completion with no object. Summary: `status:u16, object_id:u64
  (0 = none), timing_count:u16, crc32:u32, capture_complete:u8`.
- `IR_TX_RAW` (action, op 0x0411) `object_id:u64, frame_count:u8
  (=1), timeout_ms:u32`. The object must exist in the committed
  slot, belong to the session, and be complete — checked at
  request validation (consumed RESOURCE_UNAVAILABLE otherwise), so
  a partial or partial-uploaded waveform can never emit. The job
  pins the object from start to cleanup and replays it through
  the same finite provider discipline as decoded TX (supplied
  once, then Stop; completion on exhaustion + quiescing stop —
  raw signals seed the same unreachable sent-callback path in the
  1.4.3 worker). Playback is 38000 Hz / 0.330 duty, the object's
  declared replay settings. Summary mirrors decoded TX:
  `status:u16, supplied:u32, sent:u32` (trains, always 0/1 here).

Qualification: `evidence/C15-raw-ir-2026-10-08.md`. Note for
witness design: a witness armed while the operator is pressing a
remote hears the operator too — witness windows for TX verdicts
must be armed after the operator's part ends (see the C15W case).

## NFC discovery + identify (C16, ops 0x0501/0x0502)

Design: `docs/c16-nfc-design.md` (plan §10.6 steps A+B, §22.3,
§22.4). Discovery and identification only — no card content read
(step C), no emulation (step D). One `Nfc*` per job, allocated
and freed in the glue on the executor thread; scanner and poller
never coexist.

- `NFC_SCAN` (action, op 0x0501) `timeout_ms:u32` (1–60000).
  Runs the protocol scanner and reports CANDIDATES, not a card:
  each raw `NfcProtocol` is translated by ancestry
  (`nfc_protocol_has_parent(p, NfcProtocolIso14443_3a)`) to the
  portable id 1 or 0 (unmapped). Raw enum values never cross the
  seam; unmapped candidates are counted, never used as allocation
  parameters. First detection wins. Summary: `status:u16,
  mapped_count:u8, mapped[]:u16 each, unmapped_count:u8` —
  deduped, detection order, cap 4. An empty window is an OK
  completion with zero counts.
- `NFC_IDENTIFY` (action, op 0x0502) `protocol:u16,
  timeout_ms:u32`. Protocol must be 1 (ISO14443-3A); anything
  else is a consumed INVALID_ARGUMENT, refused BEFORE any NFC
  allocation. Summary (fixed 21 bytes): `status:u16, found:u8,
  protocol:u16, uid_len:u8, uid[10], atqa[2], sak:u8,
  error_events:u16`. UID lengths other than 4/7/10 are discarded
  and counted as error events; ATQA is carried in storage order,
  never reinterpreted. found=0 ⇒ every data field zero. Poller
  Error events are non-fatal and counted (a cardless field
  produces ~10/sec — platform retry behavior, diagnostic only).

Qualification: `evidence/C16-nfc-2026-10-08.md` (S50 fixture,
byte-identical identify vs the stock app; within-session
removal; mid-window cancel; 100/100 cycles).
