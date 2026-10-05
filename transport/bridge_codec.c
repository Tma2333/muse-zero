#include "bridge_codec.h"

#include <string.h>

void mb_put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void mb_put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void mb_put_u64(uint8_t* p, uint64_t v) {
    mb_put_u32(p, (uint32_t)v);
    mb_put_u32(p + 4, (uint32_t)(v >> 32));
}

size_t mb_frame_encode(
    const MbIdentity* id,
    uint8_t type,
    uint16_t op,
    uint16_t status,
    uint64_t correlation,
    uint32_t event_seq,
    const uint8_t* payload,
    size_t payload_len,
    uint8_t* wire,
    size_t wire_cap) {
    if(id == NULL || wire == NULL || payload_len > MB_PAYLOAD_MAX ||
       (payload_len != 0 && payload == NULL)) {
        return 0;
    }
    uint8_t raw[MB_FRAME_MAX];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x42; /* 'B' */
    raw[1] = 0x52; /* 'R' */
    raw[2] = 1; /* protocol major */
    raw[3] = 0; /* protocol minor */
    raw[4] = type;
    raw[5] = 0; /* flags */
    mb_put_u16(raw + 6, op);
    mb_put_u16(raw + 8, status);
    mb_put_u16(raw + 10, (uint16_t)payload_len);
    mb_put_u32(raw + 12, id->generation);
    mb_put_u64(raw + 16, correlation);
    mb_put_u32(raw + 24, event_seq);
    memcpy(raw + 28, id->boot, 16);
    memcpy(raw + 44, id->session, 16);
    if(payload_len != 0) memcpy(raw + 60, payload, payload_len);
    const size_t body = 60u + payload_len;
    mb_put_u32(raw + body, mb_crc32(raw, body));
    return mb_cobs_encode(raw, body + 4u, wire, wire_cap);
}
