#ifdef MB_REFERENCE_TEST
#ifdef NDEBUG
#error "Reference tests require assertions; do not define NDEBUG."
#endif
#include <assert.h>
#include <stdio.h>

static MbIdentity test_id(void) {
    MbIdentity id = {0}; id.boot[0] = 1; id.session[0] = 2; id.generation = 1;
    return id;
}

static void test_lease(void) {
    uint32_t ticks = 0;
    assert(mb_ticks_from_ms(500, 128, &ticks) && ticks == 64);
    assert(mb_ticks_from_ms(1, 128, &ticks) && ticks == 1);
    assert(!mb_ticks_from_ms(UINT32_MAX, 1000, &ticks));
    assert(!mb_ticks_from_ms(10, 0, &ticks));
    const MbTiming timing = {1500, 3000, 1000};
    MbIdentity id = test_id();
    MbLease l = {0};
    assert(mb_lease_commit(&l, &id, timing, 0, 0, true));
    const uint64_t c = mb_lease_challenge(&l, 500);
    assert(c == 1 && mb_lease_echo(&l, &id, c, 510, 900));
    assert(l.last_rx == 510); /* Queue delay didn't renew at t=900. */
    assert(!mb_lease_echo(&l, &id, c, 950, 950));
    const uint64_t c2 = mb_lease_challenge(&l, 1000);
    MbIdentity wrong = id; wrong.generation = 2;
    assert(!mb_lease_echo(&l, &wrong, c2, 1010, 1100));
    assert(!mb_lease_echo(&l, &id, c2, 1120, 1100)); /* Future RX timestamp. */
    assert(!mb_lease_echo(&l, &id, c2, 1010, 2001)); /* Stale queued proof. */
    mb_lease_poll(&l, 2009); assert(l.state == MB_LIVE);
    mb_lease_poll(&l, 2010); assert(l.state == MB_SUSPECT);
    const uint64_t c3 = mb_lease_challenge(&l, 3000);
    assert(!mb_lease_echo(&l, &id, c3, 3400, 3510)); /* Expiry wins. */
    assert(l.state == MB_EXPIRED && l.last_rx == 510);
    assert(!mb_lease_commit(&l, &id, timing, 3600, 3600, true));
    id.generation = 2;
    assert(mb_lease_commit(&l, &id, timing, 3600, 3600, false));
    assert(l.state == MB_LIVE); /* Link can resume while job cleanup continues. */
    assert(l.revocation_pending); /* Resume cannot erase the old stop obligation. */
    assert(!mb_lease_commit(&l, &id, timing, 3650, 3650, true));
    MbIdentity other = id; other.session[0] = 3; other.generation = 1;
    assert(!mb_lease_commit(&l, &other, timing, 3700, 3700, true));
    mb_lease_poll(&l, 6600); assert(l.state == MB_EXPIRED);
    assert(!mb_lease_commit(&l, &other, timing, 6700, 6700, false));
    assert(mb_lease_commit(&l, &other, timing, 6700, 6700, true));

    /* All arithmetic, including expiry, crosses UINT32_MAX. */
    l = (MbLease){0}; id = test_id();
    const uint32_t start = UINT32_MAX - 1000u;
    assert(mb_lease_commit(&l, &id, timing, start, start, true));
    const uint64_t cw = mb_lease_challenge(&l, start + 500u);
    assert(mb_lease_echo(&l, &id, cw, start + 510u, start + 700u));
    mb_lease_poll(&l, start + 3509u); assert(l.state == MB_SUSPECT);
    mb_lease_poll(&l, start + 3510u); assert(l.state == MB_EXPIRED);
    assert(mb_elapsed(100, UINT32_MAX - 100u, 201));
    puts("PASS lease: duplicate/stale/future proofs, expiry, resume, owner, wrap");
}

static void test_ledger(void) {
    static MbLedger l; /* Deliberately not a 9+ KiB automatic stack object. */
    const uint8_t payload[] = {7, 8, 9}, changed[] = {7, 8, 10};
    MbAdmission a = mb_ledger_admit(&l, 1, 0x7F01, payload, 3, true, MB_OK);
    assert(a.kind == MB_ADMITTED && l.high_water == 1);
    unsigned executions = 0;
    if(mb_ledger_mark_started(&l, 1)) ++executions;
    assert(!mb_ledger_mark_started(&l, 1));
    a = mb_ledger_admit(&l, 1, 0x7F01, payload, 3, false, MB_BUSY);
    assert(a.kind == MB_REPLAY && executions == 1);
    assert(mb_ledger_admit(&l, 1, 0x7F01, changed, 3, true, MB_OK).status ==
           MB_REQUEST_ID_REUSED);
    assert(mb_ledger_admit(&l, 3, 1, NULL, 0, true, MB_OK).status == MB_SEQUENCE_GAP);
    assert(mb_ledger_admit(&l, 2, 1, NULL, 0, false, MB_OK).status ==
           MB_TRY_LATER_NOT_ADMITTED && l.high_water == 1);
    a = mb_ledger_admit(&l, 2, 1, NULL, 0, true, MB_INVALID_ARGUMENT);
    assert(a.kind == MB_ADMITTED && l.high_water == 2);
    assert(mb_ledger_admit(&l, 2, 1, NULL, 0, true, MB_OK).status == MB_INVALID_ARGUMENT);
    assert(!mb_ledger_mark_started(&l, 2));
    for(uint64_t seq = 3; seq <= 20; ++seq)
        assert(mb_ledger_admit(&l, seq, 1, NULL, 0, true, MB_BUSY).kind == MB_ADMITTED);
    assert(mb_ledger_find(&l, 1) >= 0); /* Active record survived eviction pressure. */
    assert(mb_ledger_find(&l, 2) < 0);
    assert(mb_ledger_admit(&l, 2, 1, NULL, 0, true, MB_OK).status == MB_STALE_REQUEST);
    const uint8_t outcome[] = {42};
    assert(mb_ledger_finish(&l, 1, MB_OK, outcome, sizeof(outcome)));
    assert(!mb_ledger_finish(&l, 1, MB_CANCELLED, NULL, 0));
    a = mb_ledger_admit(&l, 1, 0x7F01, payload, 3, true, MB_OK);
    assert(a.kind == MB_REPLAY && l.entry[a.index].result[0] == 42 && executions == 1);
    assert(mb_ledger_admit(&l, 21, 1, NULL, 0, true, MB_BUSY).kind == MB_ADMITTED);
    assert(mb_ledger_find(&l, 1) < 0); /* Now terminal, so eligible for eviction. */
    assert(mb_ledger_admit(&l, 1, 0x7F01, payload, 3, true, MB_OK).status == MB_STALE_REQUEST);

    memset(&l, 0, sizeof(l));
    /* Synthetic pressure test of pinning, independent of the one-job gate. */
    for(uint64_t seq = 1; seq <= MB_LEDGER_SIZE; ++seq)
        assert(mb_ledger_admit(&l, seq, 1, NULL, 0, true, MB_OK).kind == MB_ADMITTED);
    assert(mb_ledger_admit(&l, 17, 1, NULL, 0, true, MB_OK).status == MB_TRY_LATER_NOT_ADMITTED);
    assert(l.high_water == 16);
    assert(mb_ledger_finish(&l, 1, MB_CANCELLED, NULL, 0));
    assert(mb_ledger_admit(&l, 17, 1, NULL, 0, true, MB_OK).kind == MB_ADMITTED);
    memset(&l, 0, sizeof(l)); l.high_water = UINT64_MAX;
    assert(mb_ledger_admit(&l, 0, 1, NULL, 0, true, MB_OK).status == MB_STALE_REQUEST);
    puts("PASS ledger: retry, conflict, gaps, reservation, eviction, pinning, terminal");
}

static uint32_t rng_state = 1;
static uint32_t test_random(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}
static void put32(uint8_t* p, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}
static size_t encode_body(uint8_t* raw, size_t body_size, uint8_t* wire) {
    put32(raw + body_size, mb_crc32(raw, body_size));
    return mb_cobs_encode(raw, body_size + 4u, wire, 515);
}

static void test_codec(void) {
    uint8_t raw[512] = {0}, wire[515], decoded[512];
    size_t used = 0;
    assert(mb_crc32((const uint8_t*)"123456789", 9) == UINT32_C(0xCBF43926));
    const uint8_t plain[] = {0, 0x11, 0}, golden[] = {1, 2, 0x11, 1};
    assert(mb_cobs_encode(plain, sizeof(plain), wire, sizeof(wire)) == sizeof(golden));
    assert(memcmp(wire, golden, sizeof(golden)) == 0);
    for(unsigned pattern = 0; pattern < 3; ++pattern) {
        for(size_t n = 0; n <= sizeof(raw); ++n) {
            for(size_t i = 0; i < n; ++i)
                raw[i] = pattern == 0 ? 0 : (pattern == 1 ? 0xFF : (uint8_t)test_random());
            const size_t length = mb_cobs_encode(raw, n, wire, sizeof(wire));
            assert(length > 0 && length <= sizeof(wire));
            assert(mb_cobs_decode(wire, length, decoded, sizeof(decoded), &used));
            assert(used == n && memcmp(raw, decoded, n) == 0);
            if(n != 0) assert(!mb_cobs_decode(wire, length, decoded, n - 1, &used));
            assert(mb_cobs_encode(raw, n, wire, length - 1) == 0);
        }
    }
    const uint8_t bad_zero[] = {2, 0}, bad_short[] = {3, 1};
    assert(!mb_cobs_decode(bad_zero, 2, decoded, sizeof(decoded), &used));
    assert(!mb_cobs_decode(bad_short, 2, decoded, sizeof(decoded), &used));
    assert(!mb_cobs_decode(wire, 0, decoded, sizeof(decoded), &used));

    /* Independent golden envelope CRC was calculated with Python zlib. */
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x42; raw[1] = 0x52; raw[2] = 1; raw[4] = 7;
    raw[6] = 1; raw[10] = 3; raw[12] = 1; raw[16] = 9;
    raw[28] = 1; raw[44] = 2; raw[60] = 2; raw[61] = 'O'; raw[62] = 'K';
    assert(mb_crc32(raw, 63) == UINT32_C(0xBECD7706));
    size_t length = encode_body(raw, 63, wire);
    MbFrame frame;
    assert(mb_frame_decode(wire, length, decoded, &frame) == MB_CODEC_OK);
    assert(frame.type == 7 && frame.op == 1 && frame.length == 3 && frame.correlation == 9);
    assert(frame.payload[0] == 2 && frame.payload[1] == 'O' && frame.payload[2] == 'K');
    MbIdentity id = test_id(); assert(mb_same_connection(&frame.identity, &id));
    raw[61] ^= 1u; /* Corruption without recalculating the CRC. */
    length = mb_cobs_encode(raw, 67, wire, sizeof(wire));
    assert(mb_frame_decode(wire, length, decoded, &frame) == MB_CODEC_CRC);
    assert(frame.payload == NULL);
    raw[61] ^= 1u; raw[5] = 1; length = encode_body(raw, 63, wire);
    assert(mb_frame_decode(wire, length, decoded, &frame) == MB_CODEC_ENVELOPE);
    raw[5] = 0; raw[2] = 2; length = encode_body(raw, 63, wire);
    assert(mb_frame_decode(wire, length, decoded, &frame) == MB_CODEC_VERSION);
    raw[2] = 1; raw[10] = 4; length = encode_body(raw, 63, wire);
    assert(mb_frame_decode(wire, length, decoded, &frame) == MB_CODEC_SIZE);
    for(unsigned round = 0; round < 10000; ++round) {
        length = test_random() % (sizeof(wire) + 1u);
        for(size_t i = 0; i < length; ++i) wire[i] = (uint8_t)test_random();
        (void)mb_frame_decode(wire, length, decoded, &frame);
    }
    puts("PASS codec: golden CRC/COBS, lengths 0..512, corruption, bounds, 10000 inputs");
}

static void test_rx(void) {
    MbRx rx;
    assert(mb_rx_init(&rx, 250));
    assert(mb_rx_byte(&rx, 0, 0) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 1, 10) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 0, 11) == MB_RX_FRAME && rx.ready_len == 1);
    assert(mb_rx_byte(&rx, 1, 20) == MB_RX_WAIT);
    assert(!mb_rx_expire(&rx, 269));
    assert(mb_rx_expire(&rx, 270) && rx.discard);
    assert(mb_rx_byte(&rx, 1, 271) == MB_RX_DISCARD);
    assert(mb_rx_byte(&rx, 0, 272) == MB_RX_DISCARD && !rx.discard);
    assert(mb_rx_byte(&rx, 1, 273) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 0, 274) == MB_RX_FRAME);
    mb_rx_poison(&rx);
    assert(mb_rx_byte(&rx, 0, 275) == MB_RX_DISCARD);
    for(unsigned i = 0; i < 515; ++i) assert(mb_rx_byte(&rx, 1, 300) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 1, 300) == MB_RX_OVERSIZE && rx.used == 0);
    assert(mb_rx_byte(&rx, 0, 301) == MB_RX_DISCARD);
    assert(mb_rx_byte(&rx, 1, 302) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 0, 552) == MB_RX_TIMEOUT && !rx.discard);
    assert(mb_rx_byte(&rx, 1, 553) == MB_RX_WAIT);
    assert(mb_rx_byte(&rx, 0, 554) == MB_RX_FRAME);
    assert(mb_rx_init(&rx, 250));
    assert(mb_rx_byte(&rx, 1, UINT32_MAX - 100u) == MB_RX_WAIT);
    assert(mb_rx_expire(&rx, 149));
    puts("PASS RX: delimiters, idle timeout, loss poison, oversize, recovery, wrap");
}

static void test_action_gate(void) {
    static MbLedger ledger;
    MbLease lease = {0};
    MbIdentity id = test_id();
    const MbTiming timing = {1500, 3000, 1000};
    const uint8_t args[] = {100, 0, 0, 0, 0, 0};
    MbFrame f = {0}; f.type = 8; f.op = 0x7F01; f.correlation = 1;
    f.identity = id; f.payload = args; f.length = sizeof(args);
    assert(mb_action_admit(&lease, &ledger, &f, 0, true, MB_OK).status == MB_NO_SESSION);
    assert(mb_session_commit_verified(&lease, &ledger, &id, timing, 0, 0, true));
    f.identity.boot[0] = 8;
    assert(mb_action_admit(&lease, &ledger, &f, 0, true, MB_OK).status == MB_NO_SESSION);
    f.identity = id; f.identity.generation = 2;
    assert(mb_action_admit(&lease, &ledger, &f, 0, true, MB_OK).status == MB_STALE_CONNECTION);
    assert(ledger.high_water == 0);
    f.identity = id;
    assert(mb_action_admit(&lease, &ledger, &f, 0, true, MB_OK).kind == MB_ADMITTED);
    assert(mb_action_admit(&lease, &ledger, &f, 1500, false, MB_BUSY).kind == MB_REPLAY);
    f.correlation = 2;
    assert(mb_action_admit(&lease, &ledger, &f, 1500, true, MB_OK).status == MB_TRY_LATER_NOT_ADMITTED);
    assert(ledger.high_water == 1);
    assert(mb_action_admit(&lease, &ledger, &f, 3000, true, MB_OK).status == MB_NO_SESSION);
    id.generation = 2;
    assert(mb_session_commit_verified(&lease, &ledger, &id, timing, 3100, 3100, true));
    assert(mb_action_admit(&lease, &ledger, &f, 3100, true, MB_OK).status == MB_STALE_CONNECTION);
    f.identity = id; f.correlation = 1;
    assert(mb_action_admit(&lease, &ledger, &f, 3100, true, MB_OK).kind == MB_REPLAY);
    assert(ledger.high_water == 1);
    f.correlation = 2;
    assert(lease.revocation_pending);
    assert(mb_action_admit(&lease, &ledger, &f, 3100, true, MB_OK).status == MB_TRY_LATER_NOT_ADMITTED);
    /* Only after controller has latched old job/pin revocation into shared state. */
    lease.revocation_pending = false;
    assert(mb_action_admit(&lease, &ledger, &f, 3100, true, MB_BUSY).status == MB_BUSY);
    assert(ledger.high_water == 2); /* Old job still owns resources; BUSY is retained. */
    id.session[0] = 3; id.generation = 1;
    assert(!mb_session_commit_verified(&lease, &ledger, &id, timing, 6200, 6200, false));
    assert(ledger.high_water == 2);
    assert(mb_session_commit_verified(&lease, &ledger, &id, timing, 6200, 6200, true));
    assert(ledger.high_water == 0 && mb_ledger_find(&ledger, 1) < 0);
    puts("PASS admission gate: identity, generation, expiry/revocation, session retention/reset");
}

int main(void) {
    test_lease(); test_ledger(); test_codec(); test_rx(); test_action_gate();
    printf("PASS all; sizeof(MbLedger)=%zu; sizeof(MbRx)=%zu\n", sizeof(MbLedger), sizeof(MbRx));
    return 0;
}
#endif
