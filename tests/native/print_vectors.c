/* Prints the golden vectors shared with tools/bridge_codec.py.
 * C and Python outputs must agree byte-for-byte (diffed in CI by hand). */
#include <stdio.h>
#include <string.h>

#include "bridge_codec.h"

static void print_hex(const char* name, const uint8_t* bytes, size_t n) {
    printf("%s ", name);
    for(size_t i = 0; i < n; ++i) printf("%02x", bytes[i]);
    printf("\n");
}

int main(void) {
    MbIdentity id = {0};
    id.boot[0] = 1;
    id.session[0] = 2;
    id.generation = 1;
    uint8_t wire[516];

    /* V1: QUERY PING, correlation 9, payload {2,'O','K'} — matches the
     * hand-built golden envelope in the reference codec test. */
    const uint8_t ping_req[] = {2, 'O', 'K'};
    size_t n = mb_frame_encode(&id, 7, 0x0001, 0, 9, 0, ping_req, sizeof(ping_req), wire, sizeof(wire));
    print_hex("V1", wire, n);

    /* V2: RESULT PING reply, correlation 9,
     * payload {3,'a','b','c', uptime=0x01020304, tick_freq=1000}. */
    uint8_t ping_resp[3 + 1 + 3 + 4 + 4];
    ping_resp[0] = 3;
    ping_resp[1] = 'a';
    ping_resp[2] = 'b';
    ping_resp[3] = 'c';
    mb_put_u32(ping_resp + 4, 0x01020304u);
    mb_put_u32(ping_resp + 8, 1000u);
    n = mb_frame_encode(&id, 9, 0x0001, 0, 9, 0, ping_resp, 12, wire, sizeof(wire));
    print_hex("V2", wire, n);

    /* V3: QUERY GET_INFO page 0, correlation 10, payload {0}. */
    const uint8_t info_req[] = {0};
    n = mb_frame_encode(&id, 7, 0x0002, 0, 10, 0, info_req, sizeof(info_req), wire, sizeof(wire));
    print_hex("V3", wire, n);
    return 0;
}
