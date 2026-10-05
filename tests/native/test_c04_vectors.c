/* C04 golden-vector tests for transport/bridge_codec.c.
 * Vectors generated independently with Python zlib.crc32 (CRC-32/ISO-HDLC,
 * check 0xCBF43926 for "123456789") and a reference COBS encoder.
 * Bootstrap frames use QUERY type 7 with zero boot/session IDs per plan 6.5.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../transport/bridge_codec.h"

static const uint8_t GOLDEN_PING_WIRE[] = {4,66,82,1,2,7,2,1,1,1,2,5,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,10,4,109,117,115,101,8,68,252,179,0};
static const uint8_t GOLDEN_INFO_WIRE[] = {4,66,82,1,2,7,2,2,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,5,173,234,204,27,0};

static void strip_delim(const uint8_t* wire, size_t n, uint8_t* out, size_t* len) {
    assert(n > 0 && wire[n - 1] == 0);
    memcpy(out, wire, n - 1);
    *len = n - 1;
}

int main(void) {
    assert(mb_crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
    uint8_t enc[600], dec[MB_FRAME_MAX];
    MbFrame f;
    size_t n;

    /* PING bootstrap: type QUERY, op 0x0001, payload len-prefixed "muse" */
    strip_delim(GOLDEN_PING_WIRE, sizeof(GOLDEN_PING_WIRE), enc, &n);
    assert(mb_frame_decode(enc, n, dec, &f) == MB_CODEC_OK);
    assert(f.type == 7 && f.op == 0x0001 && f.length == 5);
    assert(f.payload[0] == 4 && memcmp(f.payload + 1, "muse", 4) == 0);
    for(int i = 0; i < 16; i++) assert(f.identity.boot[i] == 0 && f.identity.session[i] == 0);

    /* GET_INFO page 0 */
    strip_delim(GOLDEN_INFO_WIRE, sizeof(GOLDEN_INFO_WIRE), enc, &n);
    assert(mb_frame_decode(enc, n, dec, &f) == MB_CODEC_OK);
    assert(f.type == 7 && f.op == 0x0002 && f.length == 1 && f.payload[0] == 0);

    /* Round-trip: re-encoding the decoded PING body reproduces the wire bytes */
    size_t body = 60 + 5 + 4;
    assert(mb_cobs_encode(dec, 0, enc, sizeof(enc)) == 1); /* sanity, replaced below */
    strip_delim(GOLDEN_PING_WIRE, sizeof(GOLDEN_PING_WIRE), enc, &n);
    uint8_t raw[MB_FRAME_MAX]; size_t rawlen;
    assert(mb_cobs_decode(enc, n, raw, sizeof(raw), &rawlen) && rawlen == body);
    uint8_t re[600];
    assert(mb_cobs_encode(raw, rawlen, re, sizeof(re)) == n);
    assert(memcmp(re, enc, n) == 0);

    /* Corruption: flip one payload bit -> CRC failure, no dispatch */
    raw[61] ^= 1u;
    assert(mb_cobs_encode(raw, rawlen, re, sizeof(re)) > 0);
    assert(mb_frame_decode(re, mb_cobs_encode(raw, rawlen, re, sizeof(re)), dec, &f) == MB_CODEC_CRC);

    /* Truncation and bad magic are rejected */
    assert(mb_frame_decode(enc, n - 3, dec, &f) != MB_CODEC_OK);
    raw[61] ^= 1u; raw[0] = 0x43;
    {   /* recompute CRC so only the magic is wrong */
        extern uint32_t mb_crc32(const uint8_t*, size_t);
        uint32_t c = mb_crc32(raw, rawlen - 4);
        raw[rawlen-4]=c; raw[rawlen-3]=c>>8; raw[rawlen-2]=c>>16; raw[rawlen-1]=c>>24;
        size_t rl = mb_cobs_encode(raw, rawlen, re, sizeof(re));
        assert(mb_frame_decode(re, rl, dec, &f) == MB_CODEC_ENVELOPE);
    }
    puts("PASS C04 golden vectors: CRC check value, PING/GET_INFO bootstrap, round-trip, corruption, truncation, bad magic");
    return 0;
}
