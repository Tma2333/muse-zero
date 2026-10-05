/* Muse Bridge portable reference core: C11, no Furi dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>

enum { MB_PAYLOAD_MAX = 384, MB_RESULT_MAX = 176,
       MB_FRAME_MAX = 512, MB_LEDGER_SIZE = 16, MB_PROOFS = 4 };

typedef enum {
    MB_OK = 0, MB_STOP_REQUESTED = 1, MB_INVALID_FRAME = 2,
    MB_INVALID_ARGUMENT = 3, MB_UNSUPPORTED = 4, MB_VERSION_MISMATCH = 5,
    MB_NO_SESSION = 6, MB_STALE_CONNECTION = 7, MB_SEQUENCE_GAP = 8,
    MB_REQUEST_ID_REUSED = 9, MB_STALE_REQUEST = 10, MB_BUSY = 11,
    MB_RESOURCE_UNAVAILABLE = 12, MB_LIMIT_EXCEEDED = 13, MB_TIMEOUT = 14,
    MB_CANCELLED = 15, MB_LINK_LOST = 16, MB_OVERFLOW = 17, MB_IO_ERROR = 18,
    MB_INIT_FAILED = 19, MB_CLEANUP_FAILED = 20, MB_FAULTED = 21,
    MB_TRY_LATER_NOT_ADMITTED = 22, MB_OUTCOME_UNAVAILABLE = 23
} MbStatus;

typedef struct {
    uint8_t boot[16], session[16];
    uint32_t generation;
} MbIdentity;

bool mb_same_owner(const MbIdentity* a, const MbIdentity* b) {
    return memcmp(a->boot, b->boot, 16) == 0 &&
           memcmp(a->session, b->session, 16) == 0;
}

bool mb_same_connection(const MbIdentity* a, const MbIdentity* b) {
    return mb_same_owner(a, b) && a->generation == b->generation;
}

/* All time intervals must be nonzero and strictly below 2^31 ticks. */
bool mb_ticks_from_ms(uint32_t ms, uint32_t tick_hz, uint32_t* out) {
    if(ms == 0 || tick_hz == 0 || out == NULL) return false;
    const uint64_t ticks = ((uint64_t)ms * tick_hz + 999u) / 1000u;
    if(ticks == 0 || ticks >= UINT32_C(0x80000000)) return false;
    *out = (uint32_t)ticks;
    return true;
}

bool mb_elapsed(uint32_t now, uint32_t since, uint32_t interval) {
    return (uint32_t)(now - since) >= interval;
}

/* ---- Fresh proof / sticky lease expiry; controller-owned state. ---- */
typedef enum { MB_WAITING, MB_LIVE, MB_SUSPECT, MB_EXPIRED } MbLink;
typedef struct { uint32_t suspect, expiry, proof_age; } MbTiming;
typedef struct { uint64_t counter; uint32_t issued; bool pending; } MbProof;
typedef struct {
    MbIdentity identity;
    MbTiming timing;
    MbLink state;
    uint32_t last_rx;
    uint64_t issued_counter, accepted_counter;
    unsigned next_slot;
    bool revocation_pending; /* Must survive an immediately successful resume. */
    MbProof proof[MB_PROOFS];
} MbLease;

bool mb_timing_valid(MbTiming t) {
    return t.proof_age > 0 && t.proof_age < t.suspect &&
           t.suspect < t.expiry && t.expiry < UINT32_C(0x80000000);
}

/* Also use for CLOSE_SESSION/local exit; old job stop latches remain separate. */
void mb_lease_revoke(MbLease* l) {
    if(l->state == MB_WAITING || l->state == MB_EXPIRED) return;
    l->state = MB_EXPIRED;
    l->revocation_pending = true;
}

/* Invoke every controller iteration, before processing any queued proof. */
void mb_lease_poll(MbLease* l, uint32_t now) {
    if(l->state == MB_WAITING || l->state == MB_EXPIRED) return;
    if(mb_elapsed(now, l->last_rx, l->timing.expiry)) mb_lease_revoke(l);
    else if(mb_elapsed(now, l->last_rx, l->timing.suspect)) l->state = MB_SUSPECT;
    else l->state = MB_LIVE;
}

/* Caller has verified a NEW handshake's nonce, challenge, owner policy,
 * and <= proof_age challenge-creation age. This is not HELLO handling.
 * Repeated CONFIRM must return cached READY without calling this function.
 * A different owner also requires completed old-session cleanup/reconciliation.
 */
bool mb_lease_commit(MbLease* l, const MbIdentity* id, MbTiming timing,
                     uint32_t confirm_rx, uint32_t now, bool cleanup_done) {
    mb_lease_poll(l, now);
    if(!mb_timing_valid(timing) || id->generation == 0 ||
       (uint32_t)(now - confirm_rx) > timing.proof_age) return false;
    if(l->state != MB_WAITING) {
        if(mb_same_owner(&l->identity, id)) {
            if(id->generation <= l->identity.generation) return false;
        } else if(l->state != MB_EXPIRED || !cleanup_done) return false;
    }
    const bool revocation_pending = l->revocation_pending;
    *l = (MbLease){0};
    l->identity = *id;
    l->timing = timing;
    l->state = MB_LIVE;
    l->last_rx = confirm_rx; /* Never replace arrival time with processing time. */
    l->revocation_pending = revocation_pending;
    return true;
}

/* Record creation before enqueueing CHALLENGE. Failed TX never grants a lease. */
uint64_t mb_lease_challenge(MbLease* l, uint32_t now) {
    mb_lease_poll(l, now);
    if(l->state == MB_WAITING || l->state == MB_EXPIRED) return 0;
    if(l->issued_counter == UINT64_MAX) { mb_lease_revoke(l); return 0; }
    const uint64_t counter = ++l->issued_counter;
    l->proof[l->next_slot] = (MbProof){counter, now, true};
    l->next_slot = (l->next_slot + 1u) % MB_PROOFS;
    return counter;
}

bool mb_lease_echo(MbLease* l, const MbIdentity* id, uint64_t counter,
                   uint32_t rx_tick, uint32_t now) {
    mb_lease_poll(l, now); /* Expiry wins over old packets in a queue. */
    if(l->state == MB_WAITING || l->state == MB_EXPIRED ||
       !mb_same_connection(&l->identity, id) ||
       counter <= l->accepted_counter) return false;
    const uint32_t rx_age = (uint32_t)(now - rx_tick);
    if(rx_age >= UINT32_C(0x80000000) ||
       rx_age > (uint32_t)(now - l->last_rx)) return false;
    for(unsigned i = 0; i < MB_PROOFS; ++i) {
        MbProof* p = &l->proof[i];
        if(!p->pending || p->counter != counter) continue;
        /* Both checks reject impossible ordering and stale queued echoes. */
        if((uint32_t)(rx_tick - p->issued) > l->timing.proof_age ||
           (uint32_t)(now - p->issued) > l->timing.proof_age) return false;
        p->pending = false;
        l->accepted_counter = counter;
        l->last_rx = rx_tick;
        mb_lease_poll(l, now);
        return true;
    }
    return false;
}

/* ---- Bounded action ledger; only the controller reads/writes this state. ---- */
typedef enum { MB_EMPTY, MB_ACCEPTED, MB_STARTED, MB_TERMINAL } MbEntryState;
typedef struct {
    uint64_t seq;
    uint16_t op, payload_len, result_len;
    MbStatus status;
    MbEntryState state;
    uint8_t payload[MB_PAYLOAD_MAX], result[MB_RESULT_MAX];
} MbEntry;
typedef struct { uint64_t high_water; MbEntry entry[MB_LEDGER_SIZE]; } MbLedger;
typedef enum { MB_ADMITTED, MB_REPLAY, MB_REFUSED } MbAdmitKind;
typedef struct { MbAdmitKind kind; MbStatus status; int index; } MbAdmission;

int mb_ledger_find(const MbLedger* l, uint64_t seq) {
    for(unsigned i = 0; i < MB_LEDGER_SIZE; ++i)
        if(l->entry[i].state != MB_EMPTY && l->entry[i].seq == seq) return (int)i;
    return -1;
}

/* Identity/lease gate is outside this function and MUST run first.
 * reservation_ready means reply + needed executor/buffer resources are reserved.
 * rejection is MB_OK or a terminal semantic validation result (e.g. MB_BUSY).
 * Malformed envelopes, stale identity and overload must never reach admission.
 */
MbAdmission mb_ledger_admit(MbLedger* l, uint64_t seq, uint16_t op,
                            const uint8_t* payload, size_t length,
                            bool reservation_ready, MbStatus rejection) {
    if(length > MB_PAYLOAD_MAX || (length != 0 && payload == NULL))
        return (MbAdmission){MB_REFUSED, MB_INVALID_FRAME, -1};
    const int found = mb_ledger_find(l, seq);
    if(found >= 0) {
        const MbEntry* e = &l->entry[found];
        if(e->op != op || e->payload_len != length ||
           (length != 0 && memcmp(e->payload, payload, length) != 0))
            return (MbAdmission){MB_REFUSED, MB_REQUEST_ID_REUSED, found};
        return (MbAdmission){MB_REPLAY, e->status, found};
    }
    if(seq <= l->high_water)
        return (MbAdmission){MB_REFUSED, MB_STALE_REQUEST, -1};
    /* The comparison above catches everything when high_water == UINT64_MAX. */
    if(seq != l->high_water + 1u)
        return (MbAdmission){MB_REFUSED, MB_SEQUENCE_GAP, -1};
    if(!reservation_ready)
        return (MbAdmission){MB_REFUSED, MB_TRY_LATER_NOT_ADMITTED, -1};

    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    for(unsigned i = 0; i < MB_LEDGER_SIZE; ++i) {
        const MbEntry* e = &l->entry[i];
        if(e->state == MB_EMPTY) { slot = (int)i; break; }
        if(e->state == MB_TERMINAL && (slot < 0 || e->seq < oldest)) {
            oldest = e->seq; slot = (int)i;
        }
    }
    if(slot < 0) /* Accepted/started entries are pinned, even under pressure. */
        return (MbAdmission){MB_REFUSED, MB_TRY_LATER_NOT_ADMITTED, -1};
    MbEntry* e = &l->entry[slot];
    memset(e, 0, sizeof(*e));
    e->seq = seq; e->op = op; e->payload_len = (uint16_t)length;
    if(length != 0) memcpy(e->payload, payload, length);
    e->status = rejection;
    e->state = rejection == MB_OK ? MB_ACCEPTED : MB_TERMINAL;
    l->high_water = seq; /* All ownership recorded before anything can execute. */
    return (MbAdmission){MB_ADMITTED, rejection, slot};
}

bool mb_ledger_mark_started(MbLedger* l, uint64_t seq) {
    const int index = mb_ledger_find(l, seq);
    if(index < 0 || l->entry[index].state != MB_ACCEPTED) return false;
    l->entry[index].state = MB_STARTED;
    return true;
}

/* Returns false for a duplicate terminal publication; the first result wins. */
bool mb_ledger_finish(MbLedger* l, uint64_t seq, MbStatus status,
                      const uint8_t* result, size_t length) {
    const int index = mb_ledger_find(l, seq);
    if(index < 0 || length > MB_RESULT_MAX || (length != 0 && result == NULL))
        return false;
    MbEntry* e = &l->entry[index];
    if(e->state == MB_TERMINAL) return false;
    if(length != 0) memcpy(e->result, result, length);
    e->result_len = (uint16_t)length;
    e->status = status;
    e->state = MB_TERMINAL; /* Caller has already verified hardware quiescence. */
    return true;
}

/* Controller-only commit AFTER nonce/challenge/client-policy verification.
 * Same owner: retain ledger. New owner: reset ledger only on successful commit.
 * The caller must have reconciled retained outcomes before owner replacement.
 */
bool mb_session_commit_verified(MbLease* lease, MbLedger* ledger,
                                 const MbIdentity* id, MbTiming timing,
                                 uint32_t rx_tick, uint32_t now, bool cleanup_done) {
    mb_lease_poll(lease, now);
    const bool retain = lease->state != MB_WAITING && mb_same_owner(&lease->identity, id);
    MbLease next = *lease;
    if(!mb_lease_commit(&next, id, timing, rx_tick, now, cleanup_done)) return false;
    if(!retain) memset(ledger, 0, sizeof(*ledger));
    *lease = next;
    return true;
}

/* ---- Portable COBS/CRC/frame validation for the fallback protocol only. ---- */
uint32_t mb_crc32(const uint8_t* bytes, size_t length) {
    uint32_t crc = UINT32_MAX;
    for(size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for(unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Output excludes the 0x00 delimiter; zero return means capacity failure. */
size_t mb_cobs_encode(const uint8_t* in, size_t n, uint8_t* out, size_t cap) {
    if(cap == 0 || out == NULL || (n != 0 && in == NULL)) return 0;
    size_t write = 1, code_at = 0;
    uint8_t code = 1;
    for(size_t read = 0; read < n; ++read) {
        if(in[read] == 0) {
            out[code_at] = code;
            if(write == cap) return 0;
            code_at = write++; code = 1;
        } else {
            if(write == cap) return 0;
            out[write++] = in[read];
            if(++code == UINT8_MAX) {
                out[code_at] = code;
                if(write == cap) return 0;
                code_at = write++; code = 1;
            }
        }
    }
    out[code_at] = code;
    return write;
}

bool mb_cobs_decode(const uint8_t* in, size_t n, uint8_t* out, size_t cap,
                     size_t* used) {
    if(used == NULL) return false;
    *used = 0;
    if(n == 0 || in == NULL || out == NULL) return false;
    size_t read = 0, write = 0;
    while(read < n) {
        const uint8_t code = in[read++];
        if(code == 0) return false;
        const size_t count = (size_t)code - 1u;
        if(count > n - read || count > cap - write) return false;
        for(size_t i = 0; i < count; ++i) {
            if(in[read] == 0) return false;
            out[write++] = in[read++];
        }
        if(code != UINT8_MAX && read < n) {
            if(write == cap) return false;
            out[write++] = 0;
        }
    }
    *used = write;
    return true;
}

uint16_t mb_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
uint32_t mb_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t mb_u64(const uint8_t* p) {
    return (uint64_t)mb_u32(p) | ((uint64_t)mb_u32(p + 4) << 32);
}

typedef struct {
    MbIdentity identity;
    uint8_t type;
    uint16_t op, status, length;
    uint64_t correlation;
    uint32_t event_seq;
    const uint8_t* payload; /* Borrowed from decoded buffer; copy before reuse. */
} MbFrame;
typedef enum { MB_CODEC_OK, MB_CODEC_COBS, MB_CODEC_SIZE,
               MB_CODEC_CRC, MB_CODEC_VERSION, MB_CODEC_ENVELOPE } MbCodec;

MbCodec mb_frame_decode(const uint8_t* encoded, size_t encoded_len,
                         uint8_t decoded[MB_FRAME_MAX], MbFrame* frame) {
    *frame = (MbFrame){0};
    if(encoded_len > 515u) return MB_CODEC_SIZE;
    size_t n = 0;
    if(!mb_cobs_decode(encoded, encoded_len, decoded, MB_FRAME_MAX, &n))
        return MB_CODEC_COBS;
    if(n < 64u) return MB_CODEC_SIZE;
    if(mb_crc32(decoded, n - 4u) != mb_u32(decoded + n - 4u)) return MB_CODEC_CRC;
    if(decoded[0] != 0x42 || decoded[1] != 0x52) return MB_CODEC_ENVELOPE;
    if(decoded[2] != 1 || decoded[3] != 0) return MB_CODEC_VERSION;
    if(decoded[4] < 1 || decoded[4] > 12 || decoded[5] != 0)
        return MB_CODEC_ENVELOPE;
    const uint16_t len = mb_u16(decoded + 10);
    if(len > MB_PAYLOAD_MAX || n != 60u + (size_t)len + 4u) return MB_CODEC_SIZE;
    frame->type = decoded[4]; frame->op = mb_u16(decoded + 6);
    frame->status = mb_u16(decoded + 8); frame->length = len;
    frame->identity.generation = mb_u32(decoded + 12);
    frame->correlation = mb_u64(decoded + 16);
    frame->event_seq = mb_u32(decoded + 24);
    memcpy(frame->identity.boot, decoded + 28, 16);
    memcpy(frame->identity.session, decoded + 44, 16);
    frame->payload = decoded + 60;
    /* The controller must still validate direction/type/op/schema/identity. */
    return MB_CODEC_OK;
}

/* The checked entry point for REQUEST admission. Semantic validation is pure:
 * no HAL writes, worker starts, or file mutation while computing rejection.
 * This function never queues executor work; only MB_ADMITTED + MB_OK may do so.
 */
MbAdmission mb_action_admit(MbLease* lease, MbLedger* ledger, const MbFrame* f,
                            uint32_t now, bool reservation_ready, MbStatus rejection) {
    mb_lease_poll(lease, now);
    if(f->type != 8 || f->status != 0 || f->event_seq != 0 || f->correlation == 0)
        return (MbAdmission){MB_REFUSED, MB_INVALID_FRAME, -1};
    if(lease->state == MB_WAITING || lease->state == MB_EXPIRED ||
       !mb_same_owner(&lease->identity, &f->identity))
        return (MbAdmission){MB_REFUSED, MB_NO_SESSION, -1};
    if(f->identity.generation != lease->identity.generation)
        return (MbAdmission){MB_REFUSED, MB_STALE_CONNECTION, -1};
    if(rejection != MB_OK && rejection != MB_INVALID_ARGUMENT &&
       rejection != MB_UNSUPPORTED && rejection != MB_BUSY &&
       rejection != MB_RESOURCE_UNAVAILABLE && rejection != MB_LIMIT_EXCEEDED)
        return (MbAdmission){MB_REFUSED, MB_FAULTED, -1}; /* Caller contract error. */
    return mb_ledger_admit(ledger, f->correlation, f->op, f->payload, f->length,
                           reservation_ready && lease->state == MB_LIVE &&
                           !lease->revocation_pending, rejection);
}

/* ---- Byte collector: parser-thread owned, before COBS/CRC decoding. ---- */
typedef struct {
    uint8_t encoded[515];
    size_t used, ready_len;
    uint32_t started, partial_ticks;
    bool discard;
} MbRx;
typedef enum { MB_RX_WAIT, MB_RX_FRAME, MB_RX_DISCARD,
               MB_RX_OVERSIZE, MB_RX_TIMEOUT } MbRxEvent;

bool mb_rx_init(MbRx* r, uint32_t partial_ticks) {
    if(partial_ticks == 0 || partial_ticks >= UINT32_C(0x80000000)) return false;
    *r = (MbRx){0};
    r->partial_ticks = partial_ticks;
    return true;
}

/* Invoke after any UART/ring/arrival-marker loss; don't splice across a hole. */
void mb_rx_poison(MbRx* r) {
    r->used = 0; r->ready_len = 0; r->discard = true;
}

/* Also call on the parser's bounded timed wait, even when no bytes arrive. */
bool mb_rx_expire(MbRx* r, uint32_t now) {
    if(r->used != 0 && mb_elapsed(now, r->started, r->partial_ticks)) {
        mb_rx_poison(r);
        return true;
    }
    return false;
}

MbRxEvent mb_rx_byte(MbRx* r, uint8_t byte, uint32_t now) {
    const bool timed_out = mb_rx_expire(r, now);
    r->ready_len = 0;
    if(r->discard) {
        if(byte == 0) r->discard = false;
        return timed_out ? MB_RX_TIMEOUT : MB_RX_DISCARD;
    }
    if(byte == 0) {
        if(r->used == 0) return MB_RX_WAIT; /* Ignore empty delimiters. */
        r->ready_len = r->used;
        r->used = 0;
        return MB_RX_FRAME;
    }
    if(r->used == sizeof(r->encoded)) {
        mb_rx_poison(r);
        return MB_RX_OVERSIZE;
    }
    if(r->used == 0) r->started = now;
    r->encoded[r->used++] = byte;
    return MB_RX_WAIT;
}
