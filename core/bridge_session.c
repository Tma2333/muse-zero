/*
 * bridge_session.c - C06 handshake, one-client ownership, heartbeat lease.
 * See bridge_session.h. Built on the section-21 reference lease in
 * bridge_core.h; semantics there are not re-implemented here.
 */
#include "bridge_session.h"

#include <string.h>

/* Local little-endian writers: keep the session core self-contained (the
 * shared mb_put_* writers live with the frame codec in transport/). */
static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put_u64(uint8_t* p, uint64_t v) {
    for(int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool id16_eq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 16) == 0;
}

void mb_session_init(
    MbSession* s,
    const uint8_t boot_id[16],
    const MbSessionConfig* cfg,
    MbRandomFill rng,
    void* rng_ctx) {
    memset(s, 0, sizeof(*s));
    memcpy(s->boot_id, boot_id, 16);
    s->cfg = *cfg;
    s->rng = rng;
    s->rng_ctx = rng_ctx;
    s->lease.state = MB_WAITING;
}

MbLink mb_session_poll(MbSession* s, uint32_t now) {
    mb_lease_poll(&s->lease, now);
    return s->lease.state;
}

void mb_session_revoke(MbSession* s) {
    mb_lease_revoke(&s->lease);
}

static void session_new_proposal(
    MbSession* s,
    const uint8_t client[16],
    const uint8_t nonce[16],
    bool resumed,
    uint32_t now) {
    uint8_t session_bytes[16];
    if(resumed) {
        memcpy(session_bytes, s->lease.identity.session, 16);
    } else {
        s->rng(s->rng_ctx, session_bytes, 16);
    }
    uint64_t challenge = 0;
    s->rng(s->rng_ctx, (uint8_t*)&challenge, sizeof(challenge));
    if(challenge == 0) challenge = 1;

    memcpy(s->pend_client, client, 16);
    memcpy(s->pend_nonce, nonce, 16);
    memcpy(s->pend_identity.boot, s->boot_id, 16);
    memcpy(s->pend_identity.session, session_bytes, 16);
    /* Section 7.6: a new session's first generation is 1; only a resumed
     * (retained) session increments its existing generation. */
    s->pend_identity.generation =
        resumed ? s->lease.identity.generation + 1 : 1;
    s->pend_challenge = challenge;
    s->pend_created = now;
    s->pending_valid = true;
    /* A new handshake supersedes the completed-handshake cache: a
     * duplicate CONFIRM for the old transaction must not resurrect it. */
    s->done_valid = false;
}

MbHandshakeResult mb_session_on_hello(
    MbSession* s,
    const MbLedger* ledger,
    const uint8_t client[16],
    const uint8_t nonce[16],
    const uint8_t prior_boot[16],
    const uint8_t prior_session[16],
    uint32_t now) {
    MbHandshakeResult r = {0};
    r.status = MB_OK;
    mb_lease_poll(&s->lease, now);

    /* A repeated HELLO for the already-completed handshake (its reply
     * was lost) returns the completed transaction: the client can then
     * CONFIRM and recover SESSION_READY from the cache. */
    if(s->done_valid && id16_eq(s->done_client, client) &&
       id16_eq(s->done_nonce, nonce)) {
        r.identity = s->done_identity;
        r.challenge = s->done_challenge;
        r.last_consumed_seq = ledger->high_water;
        return r;
    }

    /* Same client + nonce inside the TTL: return the pending proposal
     * unchanged (idempotent HELLO, section 7.5). */
    if(s->pending_valid && id16_eq(s->pend_client, client) &&
       id16_eq(s->pend_nonce, nonce) &&
       !mb_elapsed(now, s->pend_created, s->cfg.pending_ttl_ticks)) {
        r.identity = s->pend_identity;
        r.challenge = s->pend_challenge;
        r.last_consumed_seq = ledger->high_water;
        return r;
    }
    if(s->pending_valid &&
       mb_elapsed(now, s->pend_created, s->cfg.pending_ttl_ticks)) {
        s->pending_valid = false; /* expired proposals are dead */
    }

    /* One-client ownership. A live (or suspect) owner is never preempted.
     * A different owner after expiry still waits for completed cleanup. */
    bool foreign = s->owner_valid && !id16_eq(s->owner_client, client);
    if(foreign && (s->lease.state == MB_LIVE || s->lease.state == MB_SUSPECT)) {
        r.status = MB_BUSY;
        return r;
    }

    bool resumed = s->owner_valid && !foreign && s->lease.state != MB_WAITING &&
                   id16_eq(prior_boot, s->boot_id) &&
                   id16_eq(prior_session, s->lease.identity.session);
    session_new_proposal(s, client, nonce, resumed, now);
    r.identity = s->pend_identity;
    r.challenge = s->pend_challenge;
    r.last_consumed_seq = ledger->high_water;
    return r;
}

MbHandshakeResult mb_session_on_confirm(
    MbSession* s,
    MbLedger* ledger,
    const uint8_t client[16],
    const uint8_t nonce[16],
    uint64_t challenge,
    const MbIdentity* header_identity,
    uint32_t rx_tick,
    uint32_t now,
    bool cleanup_done) {
    MbHandshakeResult r = {0};
    mb_lease_poll(&s->lease, now);

    /* Lost-READY recovery: an exact duplicate of the completed handshake
     * returns the cached result. No re-commit, no lease renewal. */
    if(s->done_valid && id16_eq(s->done_client, client) &&
       id16_eq(s->done_nonce, nonce) && challenge == s->done_challenge &&
       mb_same_connection(&s->done_identity, header_identity)) {
        r.status = MB_OK;
        r.identity = s->done_identity;
        r.last_consumed_seq = ledger->high_water;
        return r;
    }

    if(!s->pending_valid) {
        r.status = MB_NO_SESSION;
        return r;
    }
    if(!id16_eq(s->pend_client, client) || !id16_eq(s->pend_nonce, nonce)) {
        r.status = MB_NO_SESSION; /* superseded handshake */
        return r;
    }
    if(!mb_same_connection(&s->pend_identity, header_identity)) {
        r.status = MB_STALE_CONNECTION;
        return r;
    }
    if(challenge != s->pend_challenge) {
        r.status = MB_INVALID_ARGUMENT;
        return r;
    }
    if(mb_elapsed(now, s->pend_created, s->cfg.pending_ttl_ticks) ||
       (uint32_t)(rx_tick - s->pend_created) > s->cfg.timing.proof_age) {
        s->pending_valid = false;
        r.status = MB_TIMEOUT; /* stale proof: old challenge, old proposal */
        return r;
    }
    if(!mb_session_commit_verified(
           &s->lease, ledger, &s->pend_identity, s->cfg.timing, rx_tick, now,
           cleanup_done)) {
        /* The reference commit also refuses a different owner whose old
         * cleanup is incomplete; nothing was committed. */
        r.status = MB_TRY_LATER_NOT_ADMITTED;
        return r;
    }

    memcpy(s->done_client, client, 16);
    memcpy(s->done_nonce, nonce, 16);
    s->done_identity = s->pend_identity;
    s->done_challenge = challenge;
    s->done_valid = true;
    memcpy(s->owner_client, client, 16);
    s->owner_valid = true;
    s->pending_valid = false;

    r.status = MB_OK;
    r.identity = s->done_identity;
    r.last_consumed_seq = ledger->high_water;
    return r;
}

uint64_t mb_session_challenge(MbSession* s, uint32_t now) {
    return mb_lease_challenge(&s->lease, now);
}

bool mb_session_echo(
    MbSession* s,
    const MbIdentity* header_identity,
    uint64_t counter,
    uint32_t rx_tick,
    uint32_t now) {
    return mb_lease_echo(&s->lease, header_identity, counter, rx_tick, now);
}

/* ---- wire payload codecs ---- */

bool mb_parse_hello(
    const uint8_t* p,
    size_t len,
    uint8_t client[16],
    uint8_t nonce[16],
    uint8_t prior_boot[16],
    uint8_t prior_session[16]) {
    if(len != MB_HELLO_LEN) return false;
    memcpy(client, p, 16);
    memcpy(nonce, p + 16, 16);
    memcpy(prior_boot, p + 32, 16);
    memcpy(prior_session, p + 48, 16);
    return true;
}

size_t mb_build_hello_reply(
    uint8_t* out,
    size_t cap,
    const MbSession* s,
    const MbHandshakeResult* proposal,
    uint64_t active_job_id,
    uint8_t executor_state) {
    if(cap < MB_HELLO_REPLY_LEN) return 0;
    size_t w = 0;
    memcpy(out + w, s->pend_client, 16);
    w += 16;
    memcpy(out + w, s->pend_nonce, 16);
    w += 16;
    put_u64(out + w, proposal->challenge);
    w += 8;
    put_u64(out + w, proposal->last_consumed_seq);
    w += 8;
    put_u64(out + w, active_job_id);
    w += 8;
    put_u16(out + w, s->cfg.challenge_interval_ms);
    w += 2;
    put_u16(out + w, s->cfg.proof_max_age_ms);
    w += 2;
    put_u16(out + w, s->cfg.suspect_ms);
    w += 2;
    put_u16(out + w, s->cfg.lease_ms);
    w += 2;
    put_u16(out + w, s->cfg.max_payload);
    w += 2;
    out[w++] = (uint8_t)MB_LEDGER_SIZE;
    out[w++] = (uint8_t)s->lease.state;
    out[w++] = executor_state;
    return w;
}

bool mb_parse_confirm(
    const uint8_t* p,
    size_t len,
    uint8_t client[16],
    uint8_t nonce[16],
    uint64_t* challenge) {
    if(len != MB_CONFIRM_LEN) return false;
    memcpy(client, p, 16);
    memcpy(nonce, p + 16, 16);
    *challenge = mb_u64(p + 32);
    return true;
}

size_t mb_build_ready(
    uint8_t* out,
    size_t cap,
    const MbSession* s,
    const uint8_t nonce[16],
    uint64_t last_consumed_seq,
    uint64_t active_job_id,
    uint8_t executor_state) {
    if(cap < MB_READY_LEN) return 0;
    size_t w = 0;
    memcpy(out + w, nonce, 16);
    w += 16;
    put_u64(out + w, last_consumed_seq);
    w += 8;
    put_u64(out + w, active_job_id);
    w += 8;
    out[w++] = (uint8_t)s->lease.state;
    out[w++] = executor_state;
    return w;
}
