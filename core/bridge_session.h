/*
 * bridge_session.h - C06 handshake, one-client ownership, heartbeat lease.
 *
 * Sits on the section-21 reference lease (MbLease) in bridge_core.h. This
 * layer owns the wire-facing handshake state machine:
 *
 *   HELLO -> pending proposal (one at a time, 2000 ms TTL)
 *   CONFIRM -> verified via mb_session_commit_verified; SESSION_READY
 *   duplicate CONFIRM -> cached SESSION_READY, no re-commit, no lease touch
 *   HEARTBEAT challenge/echo -> mb_lease_challenge / mb_lease_echo
 *
 * States (section 7.4): Waiting -> Handshaking -> Live -> Suspect ->
 * Expired (sticky). A live owner is never preempted by another client's
 * HELLO; a different owner needs lease expiry plus completed cleanup.
 *
 * Portable C11. Randomness (session IDs, handshake challenges) comes from
 * an injected callback so native tests stay deterministic. All times are
 * caller ticks; millisecond values are kept only for the wire reply.
 */
#ifndef BRIDGE_SESSION_H
#define BRIDGE_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_HELLO_LEN 64u
#define MB_HELLO_REPLY_LEN 69u
#define MB_CONFIRM_LEN 40u
#define MB_READY_LEN 34u
#define MB_COUNTER_LEN 8u

typedef void (*MbRandomFill)(void* ctx, uint8_t* out, size_t len);

typedef struct {
    MbTiming timing; /* suspect/expiry/proof_age, in ticks */
    uint32_t pending_ttl_ticks; /* proposal lifetime (2000 ms default) */
    uint16_t challenge_interval_ms; /* advertised on the wire */
    uint16_t proof_max_age_ms;
    uint16_t suspect_ms;
    uint16_t lease_ms;
    uint16_t max_payload; /* advertised control payload cap */
} MbSessionConfig;

typedef struct {
    MbStatus status; /* MB_OK: a proposal (HELLO) or READY (CONFIRM) */
    MbIdentity identity; /* proposal / committed identity */
    uint64_t challenge; /* proposal challenge; 0 on READY */
    uint64_t last_consumed_seq;
} MbHandshakeResult;

typedef struct {
    MbLease lease;
    uint8_t boot_id[16];
    MbSessionConfig cfg;
    MbRandomFill rng;
    void* rng_ctx;

    bool pending_valid;
    uint8_t pend_client[16];
    uint8_t pend_nonce[16];
    MbIdentity pend_identity;
    uint64_t pend_challenge;
    uint32_t pend_created;

    bool done_valid; /* completed-handshake cache for lost-READY recovery */
    uint8_t done_client[16];
    uint8_t done_nonce[16];
    MbIdentity done_identity;
    uint64_t done_challenge;

    bool owner_valid;
    uint8_t owner_client[16];
} MbSession;

void mb_session_init(
    MbSession* s,
    const uint8_t boot_id[16],
    const MbSessionConfig* cfg,
    MbRandomFill rng,
    void* rng_ctx);

/* Poll lease expiry first (controller calls this every iteration, before
 * queued proofs are considered). Returns the current link state. */
MbLink mb_session_poll(MbSession* s, uint32_t now);

/* Handle HELLO. On MB_OK the result carries the proposal to answer with;
 * the same pending proposal is returned for a repeated client+nonce.
 * MB_BUSY: a live owner exists. MB_TRY_LATER_NOT_ADMITTED: previous
 * owner's cleanup has not finished. last_consumed reflects the ledger. */
MbHandshakeResult mb_session_on_hello(
    MbSession* s,
    const MbLedger* ledger,
    const uint8_t client[16],
    const uint8_t nonce[16],
    const uint8_t prior_boot[16],
    const uint8_t prior_session[16],
    uint32_t now);

/* Handle CONFIRM. The caller has parsed client/nonce/challenge and the
 * envelope identity. Duplicate confirmations return MB_OK from the
 * completed-handshake cache without committing again or touching the
 * lease. rx_tick is the frame's arrival tick (never processing time). */
MbHandshakeResult mb_session_on_confirm(
    MbSession* s,
    MbLedger* ledger,
    const uint8_t client[16],
    const uint8_t nonce[16],
    uint64_t challenge,
    const MbIdentity* header_identity,
    uint32_t rx_tick,
    uint32_t now,
    bool cleanup_done);

/* Issue a heartbeat challenge (records creation before enqueueing, per
 * section 7.3). Returns 0 when no challenge may be issued. */
uint64_t mb_session_challenge(MbSession* s, uint32_t now);

/* Validate a heartbeat echo against the committed connection. Only a
 * fresh round trip renews the lease (proof_accepted advances). */
bool mb_session_echo(
    MbSession* s,
    const MbIdentity* header_identity,
    uint64_t counter,
    uint32_t rx_tick,
    uint32_t now);

/* CLOSE_SESSION/local exit: revoke authority; revocation_pending keeps
 * the stop obligation alive across an immediate resume (core semantics). */
void mb_session_revoke(MbSession* s);

/* ---- wire payload codecs (section 6.4 layouts) ---- */
bool mb_parse_hello(
    const uint8_t* p,
    size_t len,
    uint8_t client[16],
    uint8_t nonce[16],
    uint8_t prior_boot[16],
    uint8_t prior_session[16]);
size_t mb_build_hello_reply(
    uint8_t* out,
    size_t cap,
    const MbSession* s,
    const MbHandshakeResult* proposal,
    uint64_t active_job_id,
    uint8_t executor_state);
bool mb_parse_confirm(
    const uint8_t* p,
    size_t len,
    uint8_t client[16],
    uint8_t nonce[16],
    uint64_t* challenge);
size_t mb_build_ready(
    uint8_t* out,
    size_t cap,
    const MbSession* s,
    const uint8_t nonce[16],
    uint64_t last_consumed_seq,
    uint64_t active_job_id,
    uint8_t executor_state);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_SESSION_H */
