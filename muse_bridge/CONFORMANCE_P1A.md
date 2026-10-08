# Pi client conformance report — P1-A (2026-10-08)

Scope: the Pi-side client stack used for all qualification runs —
`expansion_transport.py` (Expansion Module Protocol session),
`bridge_codec.py` (COBS + CRC-32 framing, §21-compatible),
`bridge_hil.py` (session/lease/echo management, action driver), all
in `~/flipper-rpc/` on flippi, against release build `mb-1.0-rel-1`
and dev builds of the same protocol (1.0).

Release checklist requirement: *the Pi client distinguishes failure,
accepted/running, success, cancellation, incomplete data, and
indeterminate outcome.* Each distinction, with its demonstrating
run:

| Outcome | How the client sees it | Demonstrated |
| --- | --- | --- |
| Transport/session failure | Handshake returns no identity; connect retries 3× then raises — never mistaken for a session | C06 runs; tonight's dark-Flipper episode (launch failures surfaced, not hidden) |
| Accepted / running | Type-10 ACCEPTED frame for the action's sequence; job known in-flight | Every action case C07–C14 |
| Success | Type-12 COMPLETE, header status OK, retained summary parsed per op | C07–C13 |
| Refusal (consumed) | Type-9 RESULT with the refusal status in the header (INVALID_ARGUMENT / UNSUPPORTED / BUSY / SEQUENCE_GAP / REQUEST_ID_REUSED / STALE_*) — distinct from a completion | C07, C10–C13 rejection matrices; release smoke (FAKE_RUN → UNSUPPORTED) |
| Cancellation | COMPLETE with status CANCELLED after a CANCEL whose ack reports STOP_REQUESTED | C08, C11, C12 Job B, C14L |
| Link-lost termination | COMPLETE/GET_RESULT terminal status LINK_LOST after lease expiry + resume | C09, C12X, C13, soak injections |
| Incomplete data | IR RX event stream carries per-event `event_seq`; client reconciles received + holes == last_seq and checks the summary's emitted vs dropped — a gap is data, never a silent short count | C12 Jobs A–E |
| Indeterminate after restart | New boot ID ⇒ prior unresolved actions are reported INDETERMINATE_AFTER_RESTART (client-side state), GET_RESULT on the new boot returns OUTCOME_UNAVAILABLE; the client never auto-replays | C09R (10/10), C09 reboot variant |
| Replay safety | Byte-identical retry of an admitted action returns the retained ACCEPTED/COMPLETE; the client treats it as the same action, and the IR witness proved no second physical effect | C07, C13 |

Client discipline notes:

- The echo/heartbeat thread is independent of action traffic
  (lease proofs keep flowing while jobs run); pausing it is the
  fault-injection mechanism for L12–L17.
- Action sequences are kept monotonic across resumes; a jump is a
  correct SEQUENCE_GAP refusal (observed, handled).
- GET_RESULT is the reconciliation path after any dropped reply
  (L03) and after reconnect (L24).
- UART parameters: /dev/ttyAMA0 on the Pi 5 header, 230400 baud,
  Expansion Module Protocol wake + baud negotiation handled by the
  transport with bounded retries.

No conformance gaps were found against the P1-A feature set.
