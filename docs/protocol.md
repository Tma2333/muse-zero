# Muse Bridge wire protocol (implemented subset: C04–C11)

Status: implemented through checkpoint C04 (bootstrap only). Sessions,
the action ledger, and all hardware operations arrive in C05+; nothing
below implies they exist yet.

## Transport

- Raw USART on Flipper pins 13 (TX) / 14 (RX), 230400 8N1, owned by the
  `muse_bridge` FAP (expansion service disabled while running).
- Framing: COBS-encoded body terminated by a single 0x00 delimiter.
  Decoded frame ≤ 512 B; payload ≤ 384 B. Partial frames older than
  250 ms are discarded through the next delimiter (never spliced).

## Envelope (60-byte header, little-endian; §6.5)

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
