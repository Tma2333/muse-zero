#pragma once
/* Muse Bridge bounded raw-object store (plan 22.4, checkpoint C15).
 * Portable C11. One mutable slot (an upload in progress, or the
 * target of a running capture) and one committed slot (an immutable,
 * validated object). Fixed capacity: 512 u32 microsecond durations
 * per slot plus metadata; no allocation after init.
 *
 * Rules implemented here (the plan's, verbatim where it speaks):
 *  - object_id is the creating action's sequence.
 *  - Chunks must be contiguous; an already-written identical range
 *    is acknowledged without rewriting; conflicting or overlapping
 *    data is rejected; holes cannot commit.
 *  - Commit verifies count, CRC-32/ISO-HDLC over the canonical
 *    little-endian durations, each duration 1..1000000 us, and a
 *    total of at most 2000000 us (64-bit accumulation).
 *  - Initial replay parameters are exactly 38000 Hz / 330 permille;
 *    they are explicit settings, not measurements. Captures record
 *    carrier_measured=false and carrier/duty 0 (unknown).
 *  - An overflowing or invalid capture is retained only as
 *    explicitly incomplete diagnostic data and can never transmit.
 *  - Objects are session-owned: lease expiry keeps them, a
 *    replacing session purges them, release/session close frees
 *    them. An object pinned by a running TX is never freed.
 *  - Absolute upload lifetime is 60000 ms from BEGIN.
 */
#include "../core/bridge_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_OBJ_OP_BEGIN 0x0901
#define MB_OBJ_OP_CHUNK 0x0902
#define MB_OBJ_OP_COMMIT 0x0903
#define MB_OBJ_OP_READ 0x0904
#define MB_OBJ_OP_RELEASE 0x0905
#define MB_OBJ_OP_RX_RAW_START 0x0410
#define MB_OBJ_OP_TX_RAW 0x0411

#define MB_OBJ_TYPE_RAW_IR 1
#define MB_OBJ_MAX_TIMINGS 512
#define MB_OBJ_CHUNK_MAX 64
#define MB_OBJ_READ_MAX 32
#define MB_OBJ_REPLAY_CARRIER_HZ 38000
#define MB_OBJ_REPLAY_DUTY_PERMILLE 330
#define MB_OBJ_UPLOAD_LIFETIME_MS 60000u
#define MB_OBJ_DURATION_MIN 1u
#define MB_OBJ_DURATION_MAX 1000000u
#define MB_OBJ_TOTAL_MAX 2000000u

typedef struct {
    bool present;
    bool complete; /* validated; the only TX-able kind */
    bool from_capture;
    bool carrier_measured;
    uint64_t id;
    MbIdentity owner;
    uint16_t declared; /* expected count (upload) */
    uint16_t count; /* stored count */
    uint32_t crc; /* declared checksum (upload) / computed (committed) */
    uint32_t carrier_hz;
    uint16_t duty_permille;
    bool starts_with_mark;
    uint32_t total_us;
    uint32_t pins; /* running TX pins; release/purge must respect */
    uint32_t timings[MB_OBJ_MAX_TIMINGS];
} MbObjectSlot;

typedef struct {
    MbObjectSlot mut; /* upload in progress (mut_upload) */
    bool mut_upload;
    uint32_t mut_begin_tick;
    MbObjectSlot com; /* committed / published capture */
} MbObjectStore;

typedef struct {
    bool present;
    bool complete;
    bool from_capture;
    bool carrier_measured;
    uint16_t total_count;
    uint8_t returned;
    uint32_t crc;
    uint32_t carrier_hz;
    uint16_t duty_permille;
    bool starts_with_mark;
    uint32_t timings[MB_OBJ_READ_MAX];
} MbObjectReadResult;

void mb_object_store_init(MbObjectStore* store);

/* OBJECT_BEGIN. owner is the calling session; seq is the action
 * sequence (becomes the object id). */
MbStatus mb_object_begin(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t seq,
    uint8_t object_type,
    uint16_t timing_count,
    uint32_t carrier_hz,
    uint16_t duty_permille,
    bool starts_with_mark,
    uint32_t checksum,
    uint32_t now);

/* OBJECT_CHUNK. count 1..MB_OBJ_CHUNK_MAX durations at offset. */
MbStatus mb_object_chunk(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint16_t offset,
    uint8_t count,
    const uint32_t* durations,
    uint32_t now);

/* OBJECT_COMMIT. On success the object moves to the committed slot
 * and *out_crc (if non-NULL) receives the computed checksum. */
MbStatus mb_object_commit(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint32_t now,
    uint32_t* out_crc);

/* Publish a finished raw capture (executor-side, at job cleanup).
 * count > MB_OBJ_MAX_TIMINGS or invalid durations publish an
 * explicitly incomplete diagnostic object and return MB_OVERFLOW.
 * A busy mutable slot returns MB_BUSY. count is the worker's full
 * timing count; timings holds the first min(count, 1024) values. */
MbStatus mb_object_publish_capture(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    const uint32_t* timings,
    uint32_t count,
    uint32_t now);

/* OBJECT_READ (cached query data). */
MbStatus mb_object_read(
    const MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint16_t offset,
    uint8_t count,
    MbObjectReadResult* out);

/* OBJECT_RELEASE. */
MbStatus mb_object_release(MbObjectStore* store, const MbIdentity* owner, uint64_t id);

/* TX gate: the object must exist in the committed slot, be owned by
 * this session, and be complete. On MB_OK, the out params expose
 * the immutable data for the duration of the job (pin it). */
MbStatus mb_object_tx_check(
    const MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    const uint32_t** out_timings,
    uint16_t* out_count);

MbStatus mb_object_pin(MbObjectStore* store, uint64_t id);
void mb_object_unpin(MbObjectStore* store, uint64_t id);

/* Drop every object owned by this identity (session replaced). */
void mb_object_purge_session(MbObjectStore* store, const MbIdentity* owner);

/* True while an upload or capture owns the mutable slot. */
bool mb_object_mut_busy(const MbObjectStore* store);

/* ---- Executor module wrapping the store (OBJECT_* actions) ------
 * The store and the session-identity pointer are bound once by the
 * integrator (muse_bridge.c); both live in integrator-owned memory.
 * Each action performs its store operation in start() and completes
 * in service() with the operation's own status as the terminal
 * status, so a state refusal (BUSY, holes, CRC mismatch) is the
 * job's outcome, not an init failure. The creating action's
 * sequence (job_id) becomes the object id for BEGIN. */
typedef struct {
    uint16_t op;
    /* BEGIN */
    uint8_t object_type;
    uint16_t timing_count;
    uint32_t carrier_hz;
    uint16_t duty_permille;
    bool starts_with_mark;
    uint32_t checksum;
    /* CHUNK / COMMIT / RELEASE */
    uint64_t object_id;
    uint16_t offset;
    uint8_t count;
    uint32_t durations[MB_OBJ_CHUNK_MAX];
} MbObjParams;

typedef struct {
    MbStatus op_status;
    uint64_t object_id;
    uint32_t crc; /* commit's computed checksum; 0 otherwise */
} MbObjectOpResult;

extern const MbModule mb_module_object;

void mb_object_module_bind(
    MbObjectStore* store,
    const MbIdentity* identity,
    uint32_t (*now_ms)(void));
void mb_object_last_result(MbObjectOpResult* out);

#ifdef __cplusplus
}
#endif
