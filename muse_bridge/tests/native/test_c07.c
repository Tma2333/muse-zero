/*
 * tests/native/test_c07.c - C07 ledger + duplicate suppression composition.
 *
 * Exercises mb_action_admit / mb_ledger_admit through a committed C06
 * session: exact-duplicate replay, changed payload, sequence gap,
 * eviction staleness, consumed BUSY/invalid rejections, SUSPECT capacity
 * refusal (not consumed), pinned-ledger exhaustion and recovery, and
 * stale-envelope rejection. Execution counting itself is device-side
 * (fake_execution_count); here the admission decisions are pinned.
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
static uint8_t CLIENT[16], NONCE[16], ZEROS[16];

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

static MbFrame frame_of(Fixture* f, uint64_t seq, uint16_t op, const uint8_t* pl, uint16_t len) {
    MbFrame fr = {0};
    fr.identity = f->s.lease.identity;
    fr.type = 8;
    fr.op = op;
    fr.status = 0;
    fr.correlation = seq;
    fr.payload = pl;
    fr.length = len;
    return fr;
}

static MbAdmission admit(Fixture* f, MbFrame* fr, MbStatus rejection, bool reservation) {
    return mb_action_admit(&f->s.lease, &f->ledger, fr, 200, reservation, rejection);
}

int main(void) {
    for(int i = 0; i < 16; i++) {
        CLIENT[i] = (uint8_t)(0xA0 + i);
        NONCE[i] = (uint8_t)(0xC0 + i);
    }

    /* 1. Admit seq 1, finish it, replay returns the recorded outcome. */
    {
        Fixture f; fixture_session(&f);
        MbFrame fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        MbAdmission a = admit(&f, &fr, MB_OK, true);
        CHECK(a.kind == MB_ADMITTED && a.status == MB_OK);
        CHECK(f.ledger.high_water == 1);
        CHECK(mb_ledger_mark_started(&f.ledger, 1));
        CHECK(mb_ledger_finish(&f.ledger, 1, MB_OK, (const uint8_t*)"done", 4));
        MbAdmission replay = admit(&f, &fr, MB_OK, true);
        CHECK(replay.kind == MB_REPLAY && replay.status == MB_OK);
        CHECK(f.ledger.high_water == 1); /* nothing new consumed */

        /* 2. Same sequence, changed payload -> REQUEST_ID_REUSED. */
        uint8_t other[6] = {9, 9, 9, 9, 9, 9};
        MbFrame fr2 = frame_of(&f, 1, OP_FAKE_RUN, other, 6);
        MbAdmission reused = admit(&f, &fr2, MB_OK, true);
        CHECK(reused.kind == MB_REFUSED && reused.status == MB_REQUEST_ID_REUSED);
        CHECK(f.ledger.high_water == 1);

        /* 3. Sequence 3 before 2 -> SEQUENCE_GAP. */
        MbFrame fr3 = frame_of(&f, 3, OP_FAKE_RUN, f.payload, 6);
        MbAdmission gap = admit(&f, &fr3, MB_OK, true);
        CHECK(gap.kind == MB_REFUSED && gap.status == MB_SEQUENCE_GAP);
        CHECK(f.ledger.high_water == 1);
    }

    /* 4. BUSY rejection consumes its sequence and replays as BUSY. */
    {
        Fixture f; fixture_session(&f);
        MbFrame fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        MbAdmission a = admit(&f, &fr, MB_BUSY, true);
        CHECK(a.kind == MB_ADMITTED && a.status == MB_BUSY);
        CHECK(f.ledger.high_water == 1);
        int idx = mb_ledger_find(&f.ledger, 1);
        CHECK(idx >= 0 && f.ledger.entry[idx].state == MB_TERMINAL);
        MbAdmission replay = admit(&f, &fr, MB_OK, true);
        CHECK(replay.kind == MB_REPLAY && replay.status == MB_BUSY);
        /* A consumed invalid-argument result behaves the same way. */
        MbFrame fr2 = frame_of(&f, 2, OP_FAKE_RUN, f.payload, 3);
        MbAdmission inv = admit(&f, &fr2, MB_INVALID_ARGUMENT, true);
        CHECK(inv.kind == MB_ADMITTED && inv.status == MB_INVALID_ARGUMENT);
        CHECK(f.ledger.high_water == 2);
    }

    /* 5. Eviction: after 17 consumed actions seq 1 is stale, seq 17 replays. */
    {
        Fixture f; fixture_session(&f);
        for(uint64_t seq = 1; seq <= 17; seq++) {
            MbFrame fr = frame_of(&f, seq, OP_FAKE_RUN, f.payload, 6);
            MbAdmission a = admit(&f, &fr, MB_OK, true);
            CHECK(a.kind == MB_ADMITTED);
            CHECK(mb_ledger_finish(&f.ledger, seq, MB_OK, NULL, 0));
        }
        CHECK(f.ledger.high_water == 17);
        MbFrame old = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        MbAdmission stale = admit(&f, &old, MB_OK, true);
        CHECK(stale.kind == MB_REFUSED && stale.status == MB_STALE_REQUEST);
        MbFrame last = frame_of(&f, 17, OP_FAKE_RUN, f.payload, 6);
        MbAdmission replay = admit(&f, &last, MB_OK, true);
        CHECK(replay.kind == MB_REPLAY);
    }

    /* 6. SUSPECT: new action is TRY_LATER and consumes nothing; the
     *    identical retry after a fresh proof is admitted. */
    {
        Fixture f; fixture_session(&f);
        /* lease.last_rx=100; by t=1700 we are SUSPECT (not expired). */
        uint64_t c = mb_session_challenge(&f.s, 1600);
        CHECK(c != 0);
        MbFrame fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        fr.correlation = 1;
        MbAdmission a = mb_action_admit(&f.s.lease, &f.ledger, &fr, 1700, true, MB_OK);
        CHECK(a.kind == MB_REFUSED && a.status == MB_TRY_LATER_NOT_ADMITTED);
        CHECK(f.ledger.high_water == 0);
        CHECK(mb_session_echo(&f.s, &f.s.lease.identity, c, 1750, 1750));
        a = mb_action_admit(&f.s.lease, &f.ledger, &fr, 1800, true, MB_OK);
        CHECK(a.kind == MB_ADMITTED);
        CHECK(f.ledger.high_water == 1);
    }

    /* 7. Pinned ledger: 16 accepted (non-terminal) entries exhaust slots;
     *    a 17th distinct action is TRY_LATER and consumes nothing; once
     *    one terminates, the same retry is admitted. */
    {
        Fixture f; fixture_session(&f);
        for(uint64_t seq = 1; seq <= 16; seq++) {
            MbFrame fr = frame_of(&f, seq, OP_FAKE_RUN, f.payload, 6);
            MbAdmission a = admit(&f, &fr, MB_OK, true);
            CHECK(a.kind == MB_ADMITTED && mb_ledger_mark_started(&f.ledger, seq));
        }
        MbFrame fr17 = frame_of(&f, 17, OP_FAKE_RUN, f.payload, 6);
        MbAdmission full = admit(&f, &fr17, MB_OK, true);
        CHECK(full.kind == MB_REFUSED && full.status == MB_TRY_LATER_NOT_ADMITTED);
        CHECK(f.ledger.high_water == 16);
        CHECK(mb_ledger_finish(&f.ledger, 1, MB_OK, NULL, 0));
        MbAdmission retry = admit(&f, &fr17, MB_OK, true);
        CHECK(retry.kind == MB_ADMITTED);
        CHECK(f.ledger.high_water == 17);
    }

    /* 8. Stale envelopes refused before hardware: wrong generation,
     *    foreign owner, expired session. */
    {
        Fixture f; fixture_session(&f);
        MbFrame fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        fr.identity.generation = 99;
        MbAdmission a = admit(&f, &fr, MB_OK, true);
        CHECK(a.kind == MB_REFUSED && a.status == MB_STALE_CONNECTION);
        fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        fr.identity.session[0] ^= 0xFF;
        a = admit(&f, &fr, MB_OK, true);
        CHECK(a.kind == MB_REFUSED && a.status == MB_NO_SESSION);
        fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        a = mb_action_admit(&f.s.lease, &f.ledger, &fr, 100000, true, MB_OK);
        CHECK(a.kind == MB_REFUSED && a.status == MB_NO_SESSION); /* expired */
        CHECK(f.ledger.high_water == 0);
    }

    /* 9. Malformed action envelopes never consume. */
    {
        Fixture f; fixture_session(&f);
        MbFrame fr = frame_of(&f, 0, OP_FAKE_RUN, f.payload, 6); /* seq 0 */
        MbAdmission a = admit(&f, &fr, MB_OK, true);
        CHECK(a.kind == MB_REFUSED && a.status == MB_INVALID_FRAME);
        fr = frame_of(&f, 1, OP_FAKE_RUN, f.payload, 6);
        fr.event_seq = 3;
        a = admit(&f, &fr, MB_OK, true);
        CHECK(a.kind == MB_REFUSED && a.status == MB_INVALID_FRAME);
        CHECK(f.ledger.high_water == 0);
    }

    if(failures == 0) {
        printf("PASS test_c07 (9 groups)\n");
        return 0;
    }
    printf("FAIL test_c07: %d check(s)\n", failures);
    return 1;
}
