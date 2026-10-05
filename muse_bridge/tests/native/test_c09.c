/*
 * tests/native/test_c09.c - C09 UART loss and restart recovery.
 *
 * The section 7 recovery algorithm composed at the session/ledger
 * layer: 500 ms silence is invisible, ~2 s silence is SUSPECT (no new
 * admissions, nothing consumed), >3 s is EXPIRED with revocation, a
 * replayed-echo storm never renews a dead lease, resume preserves the
 * ledger and rotates the generation, an old-generation action after
 * resume is refused, and CONFIRM is refused (TRY_LATER) while the old
 * owner's cleanup is incomplete. Execution effects are device-side.
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

typedef struct { uint8_t next; } TestRng;
static void test_rng(void* ctx, uint8_t* out, size_t len) {
    TestRng* r = ctx;
    for(size_t i = 0; i < len; i++) out[i] = r->next++;
}

static const uint8_t BOOT[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static uint8_t CLIENT[16], NONCE[16], NONCE2[16], ZEROS[16];

static MbSessionConfig cfg = {
    .timing = {.suspect = 1500, .expiry = 3000, .proof_age = 1000},
    .pending_ttl_ticks = 2000,
    .challenge_interval_ms = 500, .proof_max_age_ms = 1000,
    .suspect_ms = 1500, .lease_ms = 3000, .max_payload = 176,
};

#define OP_FAKE_RUN 0x7F01

typedef struct {
    MbSession s;
    MbLedger ledger;
    TestRng rng;
    uint8_t payload[6];
} Fixture;

static void fixture_session(Fixture* f) {
    memset(f, 0, sizeof(*f));
    f->rng.next = 0xA0;
    mb_session_init(&f->s, BOOT, &cfg, test_rng, &f->rng);
    MbHandshakeResult p = mb_session_on_hello(
        &f->s, &f->ledger, CLIENT, NONCE, ZEROS, ZEROS, 0);
    CHECK(p.status == MB_OK);
    MbHandshakeResult c = mb_session_on_confirm(
        &f->s, &f->ledger, CLIENT, NONCE, p.challenge, &p.identity, 100, 100, true);
    CHECK(c.status == MB_OK);
    /* FAKE_RUN{duration_ms=600, interval_ms=200} */
    f->payload[0] = 0x58; f->payload[1] = 0x02; f->payload[2] = 0; f->payload[3] = 0;
    f->payload[4] = 0xC8; f->payload[5] = 0;
}

/* One fresh challenge/echo proof at tick t (must be inside proof age). */
static void prove(Fixture* f, uint32_t t) {
    uint64_t c = mb_session_challenge(&f->s, t);
    CHECK(c != 0);
    CHECK(mb_session_echo(&f->s, &f->s.lease.identity, c, t + 10, t + 10));
}

static MbFrame frame_of(Fixture* f, uint64_t seq, const uint8_t* pl, uint16_t len) {
    MbFrame fr = {0};
    fr.identity = f->s.lease.identity;
    fr.type = 8;
    fr.op = OP_FAKE_RUN;
    fr.status = 0;
    fr.correlation = seq;
    fr.payload = pl;
    fr.length = len;
    return fr;
}

static MbAdmission admit_at(Fixture* f, MbFrame* fr, uint32_t now) {
    return mb_action_admit(&f->s.lease, &f->ledger, fr, now, true, MB_OK);
}

int main(void) {
    for(int i = 0; i < 16; i++) {
        CLIENT[i] = (uint8_t)(0xA0 + i);
        NONCE[i] = (uint8_t)(0xC0 + i);
        NONCE2[i] = (uint8_t)(0xD0 + i);
    }

    /* 1. A 500 ms silence changes nothing; admission continues. */
    {
        Fixture f; fixture_session(&f);
        prove(&f, 200);
        CHECK(mb_session_poll(&f.s, 700) == MB_LIVE); /* 500 ms later */
        MbFrame fr = frame_of(&f, 1, f.payload, 6);
        MbAdmission a = admit_at(&f, &fr, 700);
        CHECK(a.kind == MB_ADMITTED);
        CHECK(f.ledger.high_water == 1);
        puts("PASS c09.1 500 ms silence: live, admission uninterrupted");
    }

    /* 2. ~2 s silence: SUSPECT refuses admission and consumes nothing;
     * a fresh proof restores admission of the same sequence. */
    {
        Fixture f; fixture_session(&f);
        prove(&f, 200);
        CHECK(mb_session_poll(&f.s, 1800) == MB_SUSPECT); /* 1600 ms */
        MbFrame fr = frame_of(&f, 1, f.payload, 6);
        MbAdmission a = admit_at(&f, &fr, 1800);
        CHECK(a.kind == MB_REFUSED && a.status == MB_TRY_LATER_NOT_ADMITTED);
        CHECK(f.ledger.high_water == 0);
        prove(&f, 1900);
        CHECK(mb_session_poll(&f.s, 1950) == MB_LIVE);
        a = admit_at(&f, &fr, 1950);
        CHECK(a.kind == MB_ADMITTED && f.ledger.high_water == 1);
        puts("PASS c09.2 suspect: refusal without consumption, proof restores");
    }

    /* 3. >3 s silence: EXPIRED, revocation pending, admission refused;
     * a storm of replayed old echoes never renews the dead lease. */
    {
        Fixture f; fixture_session(&f);
        prove(&f, 200);
        uint64_t accepted = f.s.lease.accepted_counter;
        CHECK(mb_session_poll(&f.s, 3300) == MB_EXPIRED); /* 3100 ms */
        CHECK(f.s.lease.revocation_pending);
        MbFrame fr = frame_of(&f, 1, f.payload, 6);
        MbAdmission a = admit_at(&f, &fr, 3300);
        CHECK(a.kind == MB_REFUSED && f.ledger.high_water == 0);
        for(int i = 0; i < 50; i++) {
            mb_session_echo(&f.s, &f.s.lease.identity, accepted, 3400 + i, 3400 + i);
        }
        CHECK(mb_session_poll(&f.s, 3500) == MB_EXPIRED);
        puts("PASS c09.3 expiry: revocation set, replayed echoes resurrect nothing");
    }

    /* 4. Resume after expiry: ledger preserved, generation rotated;
     * the old sequence replays, the next admits, and an action framed
     * with the pre-outage generation is refused unconsumed. */
    {
        Fixture f; fixture_session(&f);
        MbFrame f1 = frame_of(&f, 1, f.payload, 6);
        MbAdmission a = admit_at(&f, &f1, 200);
        CHECK(a.kind == MB_ADMITTED);
        CHECK(mb_ledger_finish(&f.ledger, 1, MB_OK, (const uint8_t*)"0123456789", 10));
        prove(&f, 300);
        CHECK(mb_session_poll(&f.s, 3400) == MB_EXPIRED);
        uint32_t gen_before = f.s.lease.identity.generation;

        /* Resume presents a FRESH nonce: the same nonce would return
         * the completed transaction from the done cache instead. */
        MbHandshakeResult p = mb_session_on_hello(
            &f.s, &f.ledger, CLIENT, NONCE2, BOOT, f.s.lease.identity.session, 3500);
        CHECK(p.status == MB_OK);
        CHECK(p.identity.generation == gen_before + 1);
        MbHandshakeResult c = mb_session_on_confirm(
            &f.s, &f.ledger, CLIENT, NONCE2, p.challenge, &p.identity, 3550, 3550, true);
        CHECK(c.status == MB_OK && c.last_consumed_seq == 1);

        MbFrame dup = frame_of(&f, 1, f.payload, 6);
        a = admit_at(&f, &dup, 3600);
        CHECK(a.kind == MB_REPLAY);
        MbFrame next = frame_of(&f, 2, f.payload, 6);
        /* Admission stays blocked while the old lease's revocation is
         * still pending transfer (plan 21.1); the controller clears it
         * once the executor is idle, as muse_bridge.c does. */
        MbAdmission early = admit_at(&f, &next, 3560);
        CHECK(early.kind == MB_REFUSED && early.status == MB_TRY_LATER_NOT_ADMITTED);
        f.s.lease.revocation_pending = false; /* executor idle: transferred */
        prove(&f, 3570);
        a = admit_at(&f, &next, 3600);
        CHECK(a.kind == MB_ADMITTED && f.ledger.high_water == 2);

        MbFrame stale = frame_of(&f, 3, f.payload, 6);
        stale.identity.generation = gen_before; /* the dead generation */
        a = admit_at(&f, &stale, 3600);
        CHECK(a.kind == MB_REFUSED && a.status == MB_STALE_CONNECTION);
        CHECK(f.ledger.high_water == 2);
        puts("PASS c09.4 resume: ledger kept, gen rotated, stale-gen refused");
    }

    /* 5. A NEW owner's CONFIRM while the old cleanup is incomplete
     * commits nothing (and resets nothing); the same proposal commits
     * once cleanup is done, and only then is the ledger reset. A
     * same-owner resume may commit while stopping (plan 21.1 note):
     * the executor's stop latch, not the handshake, protects the old
     * job. */
    {
        Fixture f; fixture_session(&f);
        MbFrame f1 = frame_of(&f, 1, f.payload, 6);
        MbAdmission a = admit_at(&f, &f1, 200);
        CHECK(a.kind == MB_ADMITTED);
        prove(&f, 300);
        CHECK(mb_session_poll(&f.s, 3400) == MB_EXPIRED);
        uint8_t client_b[16];
        for(int i = 0; i < 16; i++) client_b[i] = (uint8_t)(0xB0 + i);
        MbHandshakeResult p = mb_session_on_hello(
            &f.s, &f.ledger, client_b, NONCE2, ZEROS, ZEROS, 3500);
        CHECK(p.status == MB_OK);
        MbHandshakeResult c = mb_session_on_confirm(
            &f.s, &f.ledger, client_b, NONCE2, p.challenge, &p.identity, 3550, 3550, false);
        CHECK(c.status == MB_TRY_LATER_NOT_ADMITTED);
        CHECK(f.ledger.high_water == 1); /* untouched until commit */
        c = mb_session_on_confirm(
            &f.s, &f.ledger, client_b, NONCE2, p.challenge, &p.identity, 3600, 3600, true);
        CHECK(c.status == MB_OK && c.last_consumed_seq == 0);
        CHECK(f.ledger.high_water == 0); /* reset at commit */
        puts("PASS c09.5 new-owner confirm gated on cleanup completion");
    }

    if(failures) {
        printf("FAIL test_c09: %d failures\n", failures);
        return 1;
    }
    puts("PASS all C09 native tests (5 groups)");
    return 0;
}
