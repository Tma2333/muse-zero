#pragma once
/* Muse Bridge wire encoder helpers.
 * The decoder/collector live in core/bridge_core.h (extracted §21
 * reference); this header adds the matching transmit side used by the
 * FAP, native tests and the golden-vector printer. */
#include <stddef.h>
#include <stdint.h>

#include "../core/bridge_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void mb_put_u16(uint8_t* p, uint16_t v);
void mb_put_u32(uint8_t* p, uint32_t v);
void mb_put_u64(uint8_t* p, uint64_t v);

/* Builds header + payload + CRC and COBS-encodes into wire (delimiter not
 * included; caller appends the 0x00). Returns encoded length, 0 on error. */
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
    size_t wire_cap);

#ifdef __cplusplus
}
#endif
