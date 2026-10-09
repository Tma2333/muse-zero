/* C15 native tests: bounded raw-object store (plan 22.4). */
#include <stdio.h>
#include <string.h>

#include "../../core/bridge_core.h"
#include "../../modules/module_object.h"

static int failures = 0;
static int checks = 0;

#define CHECK(name, cond)                                        \
    do {                                                         \
        checks++;                                                \
        if(!(cond)) {                                            \
            failures++;                                          \
            printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
        }                                                        \
    } while(0)

static MbIdentity ident(uint8_t seed) {
    MbIdentity id;
    memset(&id, 0, sizeof(id));
    for(int i = 0; i < 16; i++) {
        id.boot[i] = (uint8_t)(seed + i);
        id.session[i] = (uint8_t)(seed * 3 + i);
    }
    id.generation = 1;
    return id;
}

static uint32_t crc_of(const uint32_t* t, uint32_t n) {
    uint8_t bytes[600 * 4];
    for(uint32_t i = 0; i < n; i++) {
        bytes[i * 4 + 0] = (uint8_t)(t[i] & 0xFFu);
        bytes[i * 4 + 1] = (uint8_t)((t[i] >> 8) & 0xFFu);
        bytes[i * 4 + 2] = (uint8_t)((t[i] >> 16) & 0xFFu);
        bytes[i * 4 + 3] = (uint8_t)((t[i] >> 24) & 0xFFu);
    }
    return mb_crc32(bytes, (size_t)n * 4u);
}

static void fill(uint32_t* t, uint32_t n, uint32_t base) {
    for(uint32_t i = 0; i < n; i++) t[i] = base + i * 10;
}

int main(void) {
    MbIdentity a = ident(1), b = ident(9);
    static MbObjectStore store;
    static uint32_t wave[600];
    MbObjectReadResult rr;
    uint32_t out_crc = 0;

    /* Group 1: begin validation. */
    mb_object_store_init(&store);
    CHECK("begin_type", mb_object_begin(&store, &a, 1, 2, 10, 38000, 330, true, 0, 100) == MB_INVALID_ARGUMENT);
    CHECK("begin_count0", mb_object_begin(&store, &a, 1, 1, 0, 38000, 330, true, 0, 100) == MB_INVALID_ARGUMENT);
    CHECK("begin_count513", mb_object_begin(&store, &a, 1, 1, 513, 38000, 330, true, 0, 100) == MB_INVALID_ARGUMENT);
    CHECK("begin_carrier", mb_object_begin(&store, &a, 1, 1, 10, 36000, 330, true, 0, 100) == MB_INVALID_ARGUMENT);
    CHECK("begin_duty", mb_object_begin(&store, &a, 1, 1, 10, 38000, 300, true, 0, 100) == MB_INVALID_ARGUMENT);
    CHECK("begin_swm", mb_object_begin(&store, &a, 1, 1, 10, 38000, 330, false, 0, 100) == MB_INVALID_ARGUMENT);
    fill(wave, 100, 500);
    uint32_t wave_crc = crc_of(wave, 100);
    CHECK("begin_ok", mb_object_begin(&store, &a, 42, 1, 100, 38000, 330, true, wave_crc, 100) == MB_OK);
    CHECK("begin_busy", mb_object_begin(&store, &a, 43, 1, 10, 38000, 330, true, 0, 100) == MB_BUSY);
    CHECK("mut_busy", mb_object_mut_busy(&store));

    /* Group 2: chunk rules. */
    CHECK("chunk_hole_first", mb_object_chunk(&store, &a, 42, 10, 4, wave, 100) == MB_INVALID_ARGUMENT);
    CHECK("chunk_ok1", mb_object_chunk(&store, &a, 42, 0, 64, wave, 100) == MB_OK);
    CHECK("chunk_ident_retry", mb_object_chunk(&store, &a, 42, 0, 64, wave, 100) == MB_OK);
    static uint32_t other[64];
    fill(other, 64, 999);
    CHECK("chunk_conflict_retry", mb_object_chunk(&store, &a, 42, 0, 64, other, 100) == MB_INVALID_ARGUMENT);
    CHECK("chunk_partial_overlap", mb_object_chunk(&store, &a, 42, 60, 8, wave + 60, 100) == MB_INVALID_ARGUMENT);
    CHECK("chunk_beyond", mb_object_chunk(&store, &a, 42, 64, 40, wave + 64, 100) == MB_INVALID_ARGUMENT);
    CHECK("chunk_rest", mb_object_chunk(&store, &a, 42, 64, 36, wave + 64, 100) == MB_OK);
    CHECK("chunk_wrong_owner", mb_object_chunk(&store, &b, 42, 0, 1, wave, 100) == MB_RESOURCE_UNAVAILABLE);
    CHECK("chunk_unknown_id", mb_object_chunk(&store, &a, 77, 0, 1, wave, 100) == MB_RESOURCE_UNAVAILABLE);
    CHECK("chunk_count0", mb_object_chunk(&store, &a, 42, 0, 0, wave, 100) == MB_INVALID_ARGUMENT);
    CHECK("chunk_count65", mb_object_chunk(&store, &a, 42, 0, 65, wave, 100) == MB_INVALID_ARGUMENT);

    /* Group 3: read in-progress + commit. */
    CHECK("read_mut", mb_object_read(&store, &a, 42, 0, 32, &rr) == MB_OK && rr.present && !rr.complete && rr.total_count == 100 && rr.returned == 32 && rr.timings[5] == wave[5]);
    CHECK("read_mut_tail", mb_object_read(&store, &a, 42, 96, 32, &rr) == MB_OK && rr.returned == 4);
    CHECK("commit_ok", mb_object_commit(&store, &a, 42, 100, &out_crc) == MB_OK);
    CHECK("commit_crc_matches_oneshot", out_crc == wave_crc);
    CHECK("commit_again", mb_object_commit(&store, &a, 42, 100, NULL) == MB_RESOURCE_UNAVAILABLE);
    CHECK("read_com", mb_object_read(&store, &a, 42, 64, 32, &rr) == MB_OK && rr.complete && rr.total_count == 100 && rr.returned == 32 && rr.timings[0] == wave[64] && rr.crc == wave_crc && !rr.from_capture && rr.carrier_hz == 38000);
    const uint32_t* tp = NULL;
    uint16_t tc = 0;
    CHECK("tx_check_ok", mb_object_tx_check(&store, &a, 42, &tp, &tc) == MB_OK && tc == 100 && tp[99] == wave[99]);

    /* Group 4: commit refusals (holes, bad CRC, bad durations). */
    mb_object_store_init(&store);
    fill(wave, 10, 100);
    CHECK("g4_begin", mb_object_begin(&store, &a, 5, 1, 10, 38000, 330, true, crc_of(wave, 10) ^ 0xFFu, 0) == MB_OK);
    CHECK("g4_partial", mb_object_chunk(&store, &a, 5, 0, 6, wave, 0) == MB_OK);
    CHECK("commit_hole", mb_object_commit(&store, &a, 5, 0, NULL) == MB_INVALID_ARGUMENT);
    CHECK("g4_rest", mb_object_chunk(&store, &a, 5, 6, 4, wave + 6, 0) == MB_OK);
    CHECK("commit_crc_mismatch", mb_object_commit(&store, &a, 5, 0, NULL) == MB_INVALID_ARGUMENT);
    mb_object_store_init(&store);
    wave[3] = 0; /* a zero duration, with the CRC declared over the
                  * corrupted array so only duration validation can
                  * refuse this commit */
    CHECK("g4z_begin", mb_object_begin(&store, &a, 5, 1, 10, 38000, 330, true, crc_of(wave, 10), 0) == MB_OK);
    CHECK("g4z_chunk", mb_object_chunk(&store, &a, 5, 0, 10, wave, 0) == MB_OK);
    CHECK("commit_zero_duration", mb_object_commit(&store, &a, 5, 0, NULL) == MB_INVALID_ARGUMENT);
    wave[3] = 130;
    mb_object_store_init(&store);
    wave[3] = 1000001;
    CHECK("g4b_begin", mb_object_begin(&store, &a, 6, 1, 10, 38000, 330, true, crc_of(wave, 10), 0) == MB_OK);
    CHECK("g4b_chunk", mb_object_chunk(&store, &a, 6, 0, 10, wave, 0) == MB_OK);
    CHECK("commit_duration_range", mb_object_commit(&store, &a, 6, 0, NULL) == MB_INVALID_ARGUMENT);
    mb_object_store_init(&store);
    static uint32_t big[3] = {700000, 700000, 700000};
    CHECK("g4c_begin", mb_object_begin(&store, &a, 7, 1, 3, 38000, 330, true, crc_of(big, 3), 0) == MB_OK);
    CHECK("g4c_chunk", mb_object_chunk(&store, &a, 7, 0, 3, big, 0) == MB_OK);
    CHECK("commit_total_over", mb_object_commit(&store, &a, 7, 0, NULL) == MB_INVALID_ARGUMENT);
    static uint32_t edge[2] = {1000000, 1000000};
    mb_object_store_init(&store);
    CHECK("g4d_begin", mb_object_begin(&store, &a, 8, 1, 2, 38000, 330, true, crc_of(edge, 2), 0) == MB_OK);
    CHECK("g4d_chunk", mb_object_chunk(&store, &a, 8, 0, 2, edge, 0) == MB_OK);
    CHECK("commit_total_edge_ok", mb_object_commit(&store, &a, 8, 0, NULL) == MB_OK);

    /* Group 5: upload expiry. */
    mb_object_store_init(&store);
    fill(wave, 10, 500);
    CHECK("g5_begin", mb_object_begin(&store, &a, 9, 1, 10, 38000, 330, true, crc_of(wave, 10), 1000) == MB_OK);
    CHECK("chunk_after_expiry", mb_object_chunk(&store, &a, 9, 0, 10, wave, 1000 + 60001) == MB_RESOURCE_UNAVAILABLE);
    CHECK("begin_after_expiry", mb_object_begin(&store, &a, 10, 1, 10, 38000, 330, true, crc_of(wave, 10), 1000 + 60002) == MB_OK);

    /* Group 6: captures. */
    mb_object_store_init(&store);
    fill(wave, 68, 900);
    CHECK("capture_ok", mb_object_publish_capture(&store, &a, 21, wave, 68, 0) == MB_OK);
    CHECK("capture_read", mb_object_read(&store, &a, 21, 0, 32, &rr) == MB_OK && rr.complete && rr.from_capture && !rr.carrier_measured && rr.carrier_hz == 0 && rr.total_count == 68);
    CHECK("capture_tx_ok", mb_object_tx_check(&store, &a, 21, &tp, &tc) == MB_OK && tc == 68);
    fill(wave, 600, 100);
    CHECK("capture_over", mb_object_publish_capture(&store, &a, 22, wave, 600, 0) == MB_OVERFLOW);
    CHECK("capture_over_read", mb_object_read(&store, &a, 22, 0, 32, &rr) == MB_OK && rr.present && !rr.complete && rr.total_count == 512);
    CHECK("capture_over_tx_refused", mb_object_tx_check(&store, &a, 22, &tp, &tc) == MB_INVALID_ARGUMENT);
    wave[0] = 0;
    CHECK("capture_bad_duration", mb_object_publish_capture(&store, &a, 23, wave, 10, 0) == MB_OVERFLOW);
    mb_object_store_init(&store);
    CHECK("g6_begin", mb_object_begin(&store, &a, 24, 1, 10, 38000, 330, true, 0, 0) == MB_OK);
    fill(wave, 68, 900);
    CHECK("capture_mut_busy", mb_object_publish_capture(&store, &a, 25, wave, 68, 0) == MB_BUSY);

    /* Group 7: pins, release, purge. */
    mb_object_store_init(&store);
    fill(wave, 10, 500);
    CHECK("g7_begin", mb_object_begin(&store, &a, 31, 1, 10, 38000, 330, true, crc_of(wave, 10), 0) == MB_OK);
    CHECK("g7_chunk", mb_object_chunk(&store, &a, 31, 0, 10, wave, 0) == MB_OK);
    CHECK("g7_commit", mb_object_commit(&store, &a, 31, 0, NULL) == MB_OK);
    CHECK("pin_ok", mb_object_pin(&store, 31) == MB_OK);
    CHECK("release_pinned", mb_object_release(&store, &a, 31) == MB_BUSY);
    mb_object_unpin(&store, 31);
    CHECK("release_ok", mb_object_release(&store, &a, 31) == MB_OK);
    CHECK("release_unknown", mb_object_release(&store, &a, 31) == MB_RESOURCE_UNAVAILABLE);
    CHECK("g7_begin_b", mb_object_begin(&store, &b, 32, 1, 10, 38000, 330, true, crc_of(wave, 10), 0) == MB_OK);
    CHECK("g7_chunk_b", mb_object_chunk(&store, &b, 32, 0, 10, wave, 0) == MB_OK);
    CHECK("g7_commit_b", mb_object_commit(&store, &b, 32, 0, NULL) == MB_OK);
    mb_object_purge_session(&store, &a); /* a owns nothing now */
    CHECK("purge_keeps_other", mb_object_read(&store, &b, 32, 0, 1, &rr) == MB_OK && rr.present);
    mb_object_purge_session(&store, &b);
    CHECK("purge_drops_owner", mb_object_read(&store, &b, 32, 0, 1, &rr) == MB_RESOURCE_UNAVAILABLE && !rr.present);

    printf("test_c15: %d checks, %d failures\n", checks, failures);
    if(failures == 0) printf("PASS test_c15 (7 groups)\n");
    return failures ? 1 : 0;
}
