/*
 * tests/native/test_c06.c - C06 handshake / ownership / heartbeat.
 *
 * Covers plan C06: one commit per fresh handshake, duplicate HELLO and
 * CONFIRM idempotency (lost-READY recovery), superseded nonce, stale
 * challenge/proposal expiry, competing-client BUSY, fresh-proof-only
 * lease renewal, suspect/expiry under an injected clock, tick wrap,
 * same-session resume (ledger retained, generation rotated), new owner
 * after expiry+cleanup (ledger reset), and the 4-challenge bound.
 */
#include <stdio.h>
#include <string.h>

#include "../../core/bridge_core.h"
#include "../../core/bridge_session.h"

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if(!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while(0)

/* Deterministic RNG: bytes 0xA0, 0xA1, ... so IDs are reproducible. */
typedef struct { uint8_t next; } TestRng;
static void test_rng(void* ctx, uint8_t* out, size_t len) {
    TestRng* r = ctx;
    for(size_t i = 0; i < len; i++) out[i] = r->next++;
}

static const uint8_t BOOT[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static uint8_t CLIENT_A[16], CLIENT_B[16], NONCE_1[16], NONCE_2[16], ZEROS[16];

static MbSessionConfig cfg = {
    .timing = {.suspect = 1500, .expiry = 3000, .proof_age = 1000},
    .pending_ttl_ticks = 2000,
    .challenge_interval_ms = 500,
    .proof_max_age_ms = 1000,
    .suspect_ms = 1500,
    .lease_ms = 3000,
    .max_payload = 176,
};

typedef struct {
    MbSession s;
    MbLedger ledger;
    TestRng rng;
} Fixture;

static void fixture_init(Fixture* f) {
    memset(f, 0, sizeof(*f));
    f->rng.next = 0xA0;
    mb_session_init(&f->s, BOOT, &cfg, test_rng, &f->rng);
}

/* Full fresh handshake for CLIENT_A/NONCE_1 at time t0; returns proposal. */
static MbHandshakeResult handshake(Fixture* f, uint32_t t0) {
    MbHandshakeResult p = mb_session_on_hello(
        &f->s, &f->ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, t0);
    CHECK(p.status == MB_OK);
    MbHandshakeResult c = mb_session_on_confirm(
        &f->s, &f->ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
        t0 + 100, t0 + 100, true);
    CHECK(c.status == MB_OK);
    return p;
}

int main(void) {
    for(int i = 0; i < 16; i++) {
        CLIENT_A[i] = (uint8_t)(0xA0 + i);
        CLIENT_B[i] = (uint8_t)(0xB0 + i);
        NONCE_1[i] = (uint8_t)(0xC0 + i);
        NONCE_2[i] = (uint8_t)(0xD0 + i);
    }

    /* 1. Fresh handshake: wrong challenge/header rejected, then commit. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 1000);
        CHECK(p.status == MB_OK);
        CHECK(p.identity.generation == 1);
        CHECK(memcmp(p.identity.boot, BOOT, 16) == 0);
        MbHandshakeResult bad = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge ^ 1, &p.identity,
            1100, 1100, true);
        CHECK(bad.status == MB_INVALID_ARGUMENT);
        MbIdentity wrong_gen = p.identity; wrong_gen.generation = 9;
        bad = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &wrong_gen,
            1100, 1100, true);
        CHECK(bad.status == MB_STALE_CONNECTION);
        CHECK(f.s.lease.state == MB_WAITING);
        MbHandshakeResult ok = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
            1100, 1100, true);
        CHECK(ok.status == MB_OK);
        CHECK(f.s.lease.state == MB_LIVE);
        CHECK(f.s.lease.identity.generation == 1);
        CHECK(f.s.lease.last_rx == 1100);

        /* 2. Duplicate HELLO returns the identical proposal. */
        MbHandshakeResult p2 = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 1200);
        CHECK(p2.status == MB_OK);
        CHECK(p2.challenge == p.challenge);
        CHECK(memcmp(&p2.identity, &p.identity, sizeof(MbIdentity)) == 0);

        /* 3. Duplicate CONFIRM: cached READY, no re-commit, no renewal. */
        f.ledger.high_water = 7; /* consumed actions must survive */
        MbHandshakeResult dup = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
            2000, 2000, true);
        CHECK(dup.status == MB_OK);
        CHECK(dup.last_consumed_seq == 7);
        CHECK(f.ledger.high_water == 7);
        CHECK(f.s.lease.last_rx == 1100); /* lease NOT renewed */
        CHECK(f.s.lease.identity.generation == 1);
    }

    /* 4. Superseded nonce: old CONFIRM rejected, new one commits. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p1 = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 0);
        MbHandshakeResult p2 = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, ZEROS, ZEROS, 10);
        CHECK(p2.status == MB_OK);
        MbHandshakeResult old = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p1.challenge, &p1.identity,
            20, 20, true);
        CHECK(old.status == MB_NO_SESSION);
        MbHandshakeResult ok = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, p2.challenge, &p2.identity,
            20, 20, true);
        CHECK(ok.status == MB_OK);
    }

    /* 5. Expired proposal and stale challenge both reject CONFIRM. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 0);
        MbHandshakeResult late = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
            2500, 2500, true);
        CHECK(late.status == MB_TIMEOUT); /* proposal TTL 2000 */

        fixture_init(&f);
        p = mb_session_on_hello(&f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 0);
        late = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
            1500, 1500, true);
        CHECK(late.status == MB_TIMEOUT); /* challenge age > proof_age */
        CHECK(f.s.lease.state == MB_WAITING);
    }

    /* 6. Competing client: BUSY, owner session untouched. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = handshake(&f, 1000);
        (void)p;
        MbHandshakeResult busy = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, ZEROS, ZEROS, 1500);
        CHECK(busy.status == MB_BUSY);
        /* Owner can still renew: challenge + fresh echo accepted. */
        uint64_t c = mb_session_challenge(&f.s, 1600);
        CHECK(c != 0);
        CHECK(mb_session_echo(&f.s, &f.s.lease.identity, c, 1650, 1650));
        CHECK(f.s.lease.state == MB_LIVE);
    }

    /* 7. Heartbeat: stale/replayed/wrong-generation echoes rejected;
     *    fresh echo rescues Suspect; expiry is sticky. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = handshake(&f, 0); /* last_rx=100 */
        uint64_t c1 = mb_session_challenge(&f.s, 200);
        CHECK(c1 != 0);
        MbIdentity wrong = p.identity; wrong.generation = 99;
        CHECK(!mb_session_echo(&f.s, &wrong, c1, 250, 250));
        CHECK(mb_session_echo(&f.s, &p.identity, c1, 250, 250));
        CHECK(!mb_session_echo(&f.s, &p.identity, c1, 260, 260)); /* replay */
        /* Suspect at +1500 from last_rx=250, fresh echo revives. */
        CHECK(mb_session_poll(&f.s, 1800) == MB_SUSPECT);
        uint64_t c2 = mb_session_challenge(&f.s, 1800);
        CHECK(c2 != 0 && c2 != c1);
        CHECK(mb_session_echo(&f.s, &p.identity, c2, 1850, 1850));
        CHECK(f.s.lease.state == MB_LIVE);
        /* Expiry at +3000 from 1850; late echo cannot revive. */
        uint64_t c3 = mb_session_challenge(&f.s, 4000);
        CHECK(mb_session_poll(&f.s, 4900) == MB_EXPIRED);
        CHECK(!mb_session_echo(&f.s, &p.identity, c3, 4900, 4900));
        CHECK(f.s.lease.state == MB_EXPIRED);
        /* Duplicate CONFIRM still returns cached READY (informational),
         * but the link state it reports is Expired. */
        MbHandshakeResult dup = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, p.challenge, &p.identity,
            4950, 4950, true);
        CHECK(dup.status == MB_OK);
        CHECK(f.s.lease.state == MB_EXPIRED);
    }

    /* 8. Tick wrap: suspect/expiry and echo math survive rollover. */
    {
        Fixture f; fixture_init(&f);
        uint32_t t0 = 0xFFFFF000u;
        MbHandshakeResult p = handshake(&f, t0);
        uint64_t c = mb_session_challenge(&f.s, t0 + 200);
        uint32_t rx = t0 + 500; /* wraps past 2^32 */
        CHECK(mb_session_echo(&f.s, &p.identity, c, rx, rx));
        CHECK(mb_session_poll(&f.s, rx + 1600) == MB_SUSPECT);
        CHECK(mb_session_poll(&f.s, rx + 3100) == MB_EXPIRED);
    }

    /* 9. Resume: same session retained, ledger kept, gen rotated. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = handshake(&f, 0);
        f.ledger.high_water = 7;
        CHECK(mb_session_poll(&f.s, 10000) == MB_EXPIRED);
        MbHandshakeResult rp = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, BOOT, p.identity.session, 10100);
        CHECK(rp.status == MB_OK);
        CHECK(memcmp(rp.identity.session, p.identity.session, 16) == 0);
        CHECK(rp.identity.generation == p.identity.generation + 1);
        MbHandshakeResult rc = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, rp.challenge, &rp.identity,
            10200, 10200, true);
        CHECK(rc.status == MB_OK);
        CHECK(f.ledger.high_water == 7); /* retained */
        CHECK(rc.last_consumed_seq == 7);
        /* Old-generation echo rejected after rotation. */
        uint64_t c = mb_session_challenge(&f.s, 10300);
        CHECK(!mb_session_echo(&f.s, &p.identity, c, 10350, 10350));
        CHECK(mb_session_echo(&f.s, &rp.identity, c, 10350, 10350));
    }

    /* 10. New owner after expiry: cleanup gate, then ledger reset. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = handshake(&f, 0);
        (void)p;
        f.ledger.high_water = 9;
        CHECK(mb_session_poll(&f.s, 10000) == MB_EXPIRED);
        MbHandshakeResult bp = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, ZEROS, ZEROS, 10100);
        CHECK(bp.status == MB_OK);
        CHECK(memcmp(bp.identity.session, p.identity.session, 16) != 0);
        CHECK(bp.identity.generation == 1);
        MbHandshakeResult blocked = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, bp.challenge, &bp.identity,
            10200, 10200, false); /* cleanup not done */
        CHECK(blocked.status == MB_TRY_LATER_NOT_ADMITTED);
        /* Idempotent HELLO returns the same live proposal for retry. */
        MbHandshakeResult bp2 = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, ZEROS, ZEROS, 10300);
        CHECK(bp2.challenge == bp.challenge);
        MbHandshakeResult ok = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, bp2.challenge, &bp2.identity,
            10400, 10400, true);
        CHECK(ok.status == MB_OK);
        CHECK(f.ledger.high_water == 0); /* new owner resets the ledger */
    }

    /* 11. Owner cannot replace its own live session identity. */
    {
        Fixture f; fixture_init(&f);
        handshake(&f, 0);
        MbHandshakeResult np = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, ZEROS, ZEROS, 500);
        CHECK(np.status == MB_OK); /* proposal offered... */
        MbHandshakeResult nc = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_A, NONCE_2, np.challenge, &np.identity,
            600, 600, true);
        CHECK(nc.status == MB_TRY_LATER_NOT_ADMITTED); /* ...but no commit */
        CHECK(f.s.lease.identity.generation == 1);
    }

    /* 12. Four outstanding challenges: the evicted counter is dead. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = handshake(&f, 0);
        uint64_t cs[5];
        for(int i = 0; i < 5; i++) {
            cs[i] = mb_session_challenge(&f.s, 200 + (uint32_t)i * 10);
            CHECK(cs[i] != 0);
        }
        CHECK(!mb_session_echo(&f.s, &p.identity, cs[0], 300, 300));
        CHECK(mb_session_echo(&f.s, &p.identity, cs[4], 300, 300));
        CHECK(mb_session_poll(&f.s, 400) == MB_LIVE);
    }

    /* 13. Wire codecs: exact lengths and field placement. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult p = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, ZEROS, ZEROS, 0);
        uint8_t out[80];
        size_t n = mb_build_hello_reply(out, sizeof(out), &f.s, &p, 0, 1);
        CHECK(n == MB_HELLO_REPLY_LEN);
        CHECK(memcmp(out, CLIENT_A, 16) == 0);
        CHECK(memcmp(out + 16, NONCE_1, 16) == 0);
        CHECK(mb_u64(out + 32) == p.challenge);
        CHECK(mb_u16(out + 56) == 500);   /* challenge_interval_ms */
        CHECK(mb_u16(out + 62) == 3000);  /* lease_ms */
        CHECK(out[66] == MB_LEDGER_SIZE);
        CHECK(out[67] == (uint8_t)MB_WAITING);
        CHECK(out[68] == 1);              /* executor_state passthrough */

        uint8_t client[16], nonce[16], pb[16], ps[16];
        uint8_t hello[MB_HELLO_LEN];
        memcpy(hello, CLIENT_A, 16); memcpy(hello + 16, NONCE_1, 16);
        memcpy(hello + 32, BOOT, 16); memcpy(hello + 48, BOOT, 16);
        CHECK(mb_parse_hello(hello, sizeof(hello), client, nonce, pb, ps));
        CHECK(memcmp(client, CLIENT_A, 16) == 0 && memcmp(pb, BOOT, 16) == 0);
        CHECK(!mb_parse_hello(hello, 63, client, nonce, pb, ps));

        uint8_t confirm[MB_CONFIRM_LEN];
        memcpy(confirm, CLIENT_A, 16); memcpy(confirm + 16, NONCE_1, 16);
        uint64_t ch = 0;
        /* write the challenge little-endian as the wire does */
        for(int i = 0; i < 8; i++) confirm[32 + i] = (uint8_t)(p.challenge >> (8 * i));
        CHECK(mb_parse_confirm(confirm, sizeof(confirm), client, nonce, &ch));
        CHECK(ch == p.challenge);

        n = mb_build_ready(out, sizeof(out), &f.s, NONCE_1, 42, 0, 2);
        CHECK(n == MB_READY_LEN);
        CHECK(mb_u64(out + 16) == 42);
    }

    /* 14. A replaced session cannot be resurrected by its old owner;
     *    the replacing owner's session is the retained one. */
    {
        Fixture f; fixture_init(&f);
        MbHandshakeResult pa = handshake(&f, 0);
        CHECK(mb_session_poll(&f.s, 10000) == MB_EXPIRED);
        MbHandshakeResult bp = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, ZEROS, ZEROS, 10100);
        CHECK(bp.status == MB_OK);
        MbHandshakeResult bc2 = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT_B, NONCE_2, bp.challenge, &bp.identity,
            10200, 10200, true);
        CHECK(bc2.status == MB_OK);
        CHECK(mb_session_poll(&f.s, 20000) == MB_EXPIRED);
        /* Old owner A claims its destroyed session: new identity, gen 1. */
        MbHandshakeResult zombie = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_A, NONCE_1, BOOT, pa.identity.session, 20100);
        CHECK(zombie.status == MB_OK);
        CHECK(memcmp(zombie.identity.session, pa.identity.session, 16) != 0);
        CHECK(zombie.identity.generation == 1);
        /* Current owner B resumes the retained session at gen 2. */
        MbHandshakeResult rb = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT_B, NONCE_1, BOOT, bp.identity.session, 20200);
        CHECK(rb.status == MB_OK);
        CHECK(memcmp(rb.identity.session, bp.identity.session, 16) == 0);
        CHECK(rb.identity.generation == 2);
    }

    if(failures == 0) {
        printf("PASS test_c06 (14 groups)\n");
        return 0;
    }
    printf("FAIL test_c06: %d check(s)\n", failures);
    return 1;
}
