/* Muse Bridge bounded raw-object store (plan 22.4, C15). */
#include "module_object.h"

#include <string.h>

static bool identity_equal(const MbIdentity* a, const MbIdentity* b) {
    return memcmp(a->boot, b->boot, 16) == 0 && memcmp(a->session, b->session, 16) == 0 &&
           a->generation == b->generation;
}

static void slot_clear(MbObjectSlot* slot) {
    memset(slot, 0, sizeof(*slot));
}

/* Streaming form of mb_crc32 (same reflected ISO-HDLC convention:
 * state starts at UINT32_MAX, the published value is ~state). */
static uint32_t crc_update(uint32_t state, const uint8_t* bytes, size_t n) {
    uint32_t crc = state;
    for(size_t i = 0; i < n; ++i) {
        crc ^= bytes[i];
        for(unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xEDB88320) & (0u - (crc & 1u)));
    }
    return crc;
}

/* CRC-32/ISO-HDLC over the canonical little-endian durations. */
static uint32_t crc_of_timings(const uint32_t* timings, uint32_t count) {
    uint32_t state = UINT32_MAX;
    for(uint32_t i = 0; i < count; i++) {
        uint8_t le[4];
        le[0] = (uint8_t)(timings[i] & 0xFFu);
        le[1] = (uint8_t)((timings[i] >> 8) & 0xFFu);
        le[2] = (uint8_t)((timings[i] >> 16) & 0xFFu);
        le[3] = (uint8_t)((timings[i] >> 24) & 0xFFu);
        state = crc_update(state, le, 4);
    }
    return ~state;
}

static bool durations_valid(const uint32_t* timings, uint32_t count, uint64_t* out_total) {
    uint64_t total = 0;
    for(uint32_t i = 0; i < count; i++) {
        if(timings[i] < MB_OBJ_DURATION_MIN || timings[i] > MB_OBJ_DURATION_MAX) return false;
        total += timings[i];
        if(total > MB_OBJ_TOTAL_MAX) return false;
    }
    if(out_total) *out_total = total;
    return true;
}

/* An expired upload silently returns its slot (22.4: absolute 60 s
 * upload lifetime). Only the mutable upload slot can expire. */
static void expire_upload(MbObjectStore* store, uint32_t now) {
    if(store->mut.present && store->mut_upload &&
       (uint32_t)(now - store->mut_begin_tick) > MB_OBJ_UPLOAD_LIFETIME_MS) {
        slot_clear(&store->mut);
        store->mut_upload = false;
    }
}

void mb_object_store_init(MbObjectStore* store) {
    memset(store, 0, sizeof(*store));
}

bool mb_object_mut_busy(const MbObjectStore* store) {
    return store->mut.present;
}

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
    uint32_t now) {
    expire_upload(store, now);
    if(object_type != MB_OBJ_TYPE_RAW_IR) return MB_INVALID_ARGUMENT;
    if(timing_count == 0 || timing_count > MB_OBJ_MAX_TIMINGS) return MB_INVALID_ARGUMENT;
    if(carrier_hz != MB_OBJ_REPLAY_CARRIER_HZ) return MB_INVALID_ARGUMENT;
    if(duty_permille != MB_OBJ_REPLAY_DUTY_PERMILLE) return MB_INVALID_ARGUMENT;
    if(!starts_with_mark) return MB_INVALID_ARGUMENT;
    if(store->mut.present) return MB_BUSY;
    slot_clear(&store->mut);
    store->mut.present = true;
    store->mut.complete = false;
    store->mut.from_capture = false;
    store->mut.carrier_measured = false;
    store->mut.id = seq;
    store->mut.owner = *owner;
    store->mut.declared = timing_count;
    store->mut.count = 0;
    store->mut.crc = checksum;
    store->mut.carrier_hz = carrier_hz;
    store->mut.duty_permille = duty_permille;
    store->mut.starts_with_mark = true;
    store->mut_upload = true;
    store->mut_begin_tick = now;
    return MB_OK;
}

MbStatus mb_object_chunk(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint16_t offset,
    uint8_t count,
    const uint32_t* durations,
    uint32_t now) {
    expire_upload(store, now);
    if(count == 0 || count > MB_OBJ_CHUNK_MAX || durations == NULL) return MB_INVALID_ARGUMENT;
    if(!store->mut.present || !store->mut_upload) return MB_RESOURCE_UNAVAILABLE;
    if(store->mut.id != id || !identity_equal(&store->mut.owner, owner))
        return MB_RESOURCE_UNAVAILABLE;
    uint32_t end = (uint32_t)offset + count;
    if(end > store->mut.declared) return MB_INVALID_ARGUMENT;
    if(offset == store->mut.count) {
        memcpy(&store->mut.timings[offset], durations, (size_t)count * sizeof(uint32_t));
        store->mut.count = (uint16_t)end;
        return MB_OK;
    }
    if(end <= store->mut.count) {
        /* Already-written range: acknowledge only if byte-identical. */
        if(memcmp(&store->mut.timings[offset], durations, (size_t)count * sizeof(uint32_t)) == 0)
            return MB_OK;
        return MB_INVALID_ARGUMENT;
    }
    return MB_INVALID_ARGUMENT; /* hole ahead, or partial overlap */
}

MbStatus mb_object_commit(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint32_t now,
    uint32_t* out_crc) {
    expire_upload(store, now);
    if(!store->mut.present || !store->mut_upload) return MB_RESOURCE_UNAVAILABLE;
    if(store->mut.id != id || !identity_equal(&store->mut.owner, owner))
        return MB_RESOURCE_UNAVAILABLE;
    if(store->mut.count != store->mut.declared) return MB_INVALID_ARGUMENT; /* holes */
    uint64_t total = 0;
    if(!durations_valid(store->mut.timings, store->mut.count, &total))
        return MB_INVALID_ARGUMENT;
    uint32_t crc = crc_of_timings(store->mut.timings, store->mut.count);
    if(crc != store->mut.crc) return MB_INVALID_ARGUMENT; /* checksum mismatch */
    store->com = store->mut;
    store->com.complete = true;
    store->com.crc = crc;
    store->com.total_us = (uint32_t)total;
    slot_clear(&store->mut);
    store->mut_upload = false;
    if(out_crc) *out_crc = crc;
    return MB_OK;
}

MbStatus mb_object_publish_capture(
    MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    const uint32_t* timings,
    uint32_t count,
    uint32_t now) {
    (void)now;
    if(count == 0 || timings == NULL) return MB_INVALID_ARGUMENT;
    if(store->mut.present) return MB_BUSY;
    uint64_t total = 0;
    bool fits = count <= MB_OBJ_MAX_TIMINGS;
    bool valid = fits && durations_valid(timings, count, &total);
    slot_clear(&store->com);
    store->com.present = true;
    store->com.from_capture = true;
    store->com.carrier_measured = false;
    store->com.carrier_hz = 0; /* unknown on RX, never the replay default */
    store->com.duty_permille = 0;
    store->com.starts_with_mark = true;
    store->com.id = id;
    store->com.owner = *owner;
    uint32_t stored = fits ? count : MB_OBJ_MAX_TIMINGS;
    memcpy(store->com.timings, timings, (size_t)stored * sizeof(uint32_t));
    store->com.count = (uint16_t)stored;
    store->com.declared = store->com.count;
    if(valid) {
        store->com.crc = crc_of_timings(timings, count);
        store->com.total_us = (uint32_t)total;
        store->com.complete = true;
        return MB_OK;
    }
    store->com.crc = 0;
    store->com.complete = false; /* explicitly incomplete diagnostic */
    return MB_OVERFLOW;
}

MbStatus mb_object_read(
    const MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    uint16_t offset,
    uint8_t count,
    MbObjectReadResult* out) {
    if(out == NULL || count == 0 || count > MB_OBJ_READ_MAX) return MB_INVALID_ARGUMENT;
    const MbObjectSlot* slot = NULL;
    if(store->com.present && store->com.id == id) slot = &store->com;
    if(store->mut.present && store->mut.id == id) slot = &store->mut;
    if(slot == NULL || !identity_equal(&slot->owner, owner)) {
        memset(out, 0, sizeof(*out));
        return MB_RESOURCE_UNAVAILABLE;
    }
    memset(out, 0, sizeof(*out));
    out->present = true;
    out->complete = slot->complete;
    out->from_capture = slot->from_capture;
    out->carrier_measured = slot->carrier_measured;
    out->total_count = slot->complete || slot->from_capture ? slot->count : slot->declared;
    out->crc = slot->crc;
    out->carrier_hz = slot->carrier_hz;
    out->duty_permille = slot->duty_permille;
    out->starts_with_mark = slot->starts_with_mark;
    uint32_t avail = 0;
    if(offset < slot->count) avail = (uint32_t)slot->count - offset;
    uint32_t n = avail < count ? avail : count;
    out->returned = (uint8_t)n;
    if(n > 0) memcpy(out->timings, &slot->timings[offset], (size_t)n * sizeof(uint32_t));
    return MB_OK;
}

MbStatus mb_object_release(MbObjectStore* store, const MbIdentity* owner, uint64_t id) {
    if(store->com.present && store->com.id == id) {
        if(!identity_equal(&store->com.owner, owner)) return MB_RESOURCE_UNAVAILABLE;
        if(store->com.pins > 0) return MB_BUSY;
        slot_clear(&store->com);
        return MB_OK;
    }
    if(store->mut.present && store->mut.id == id) {
        if(!identity_equal(&store->mut.owner, owner)) return MB_RESOURCE_UNAVAILABLE;
        slot_clear(&store->mut);
        store->mut_upload = false;
        return MB_OK;
    }
    return MB_RESOURCE_UNAVAILABLE;
}

MbStatus mb_object_tx_check(
    const MbObjectStore* store,
    const MbIdentity* owner,
    uint64_t id,
    const uint32_t** out_timings,
    uint16_t* out_count) {
    if(!store->com.present || store->com.id != id) return MB_RESOURCE_UNAVAILABLE;
    if(!identity_equal(&store->com.owner, owner)) return MB_RESOURCE_UNAVAILABLE;
    if(!store->com.complete) return MB_INVALID_ARGUMENT; /* partial never emits */
    if(out_timings) *out_timings = store->com.timings;
    if(out_count) *out_count = store->com.count;
    return MB_OK;
}

MbStatus mb_object_pin(MbObjectStore* store, uint64_t id) {
    if(store->com.present && store->com.id == id) {
        store->com.pins++;
        return MB_OK;
    }
    return MB_RESOURCE_UNAVAILABLE;
}

void mb_object_unpin(MbObjectStore* store, uint64_t id) {
    if(store->com.present && store->com.id == id && store->com.pins > 0) store->com.pins--;
}

void mb_object_purge_session(MbObjectStore* store, const MbIdentity* owner) {
    if(store->mut.present && identity_equal(&store->mut.owner, owner)) {
        slot_clear(&store->mut);
        store->mut_upload = false;
    }
    if(store->com.present && identity_equal(&store->com.owner, owner) && store->com.pins == 0) {
        slot_clear(&store->com);
    }
}

/* ---- Executor module wrapping the store ---- */
static MbObjectStore* obj_store;
static const MbIdentity* obj_identity;
static uint32_t (*obj_now_ms)(void);
static MbObjectOpResult obj_last;

void mb_object_module_bind(
    MbObjectStore* store,
    const MbIdentity* identity,
    uint32_t (*now_ms)(void)) {
    obj_store = store;
    obj_identity = identity;
    obj_now_ms = now_ms;
}

void mb_object_last_result(MbObjectOpResult* out) {
    if(out) *out = obj_last;
}

typedef struct {
    MbObjParams params;
    MbStatus op_status;
    uint32_t crc;
    bool performed;
} MbObjectState;

static MbStatus object_validate_fn(const void* params) {
    const MbObjParams* p = params;
    if(p == NULL || obj_store == NULL || obj_identity == NULL) return MB_INVALID_ARGUMENT;
    switch(p->op) {
    case MB_OBJ_OP_BEGIN:
    case MB_OBJ_OP_CHUNK:
    case MB_OBJ_OP_COMMIT:
    case MB_OBJ_OP_RELEASE:
        return MB_OK;
    default:
        return MB_INVALID_ARGUMENT;
    }
}

static MbStatus object_start(MbJobContext* job, const void* params) {
    MbObjectState* s = job->module_state;
    const MbObjParams* p = params;
    if(s == NULL || p == NULL || obj_store == NULL || obj_now_ms == NULL) return MB_INIT_FAILED;
    s->params = *p;
    /* The operations are instantaneous: perform in start so the
     * outcome is fixed before the first service tick. */
    uint32_t t = obj_now_ms();
    MbStatus st = MB_INVALID_ARGUMENT;
    uint32_t crc = 0;
    switch(p->op) {
    case MB_OBJ_OP_BEGIN:
        st = mb_object_begin(
            obj_store, obj_identity, job->job_id, p->object_type, p->timing_count,
            p->carrier_hz, p->duty_permille, p->starts_with_mark, p->checksum, t);
        break;
    case MB_OBJ_OP_CHUNK:
        st = mb_object_chunk(
            obj_store, obj_identity, p->object_id, p->offset, p->count, p->durations, t);
        break;
    case MB_OBJ_OP_COMMIT:
        st = mb_object_commit(obj_store, obj_identity, p->object_id, t, &crc);
        break;
    case MB_OBJ_OP_RELEASE:
        st = mb_object_release(obj_store, obj_identity, p->object_id);
        break;
    default:
        break;
    }
    s->op_status = st;
    s->crc = crc;
    s->performed = true;
    obj_last.op_status = st;
    obj_last.object_id = (p->op == MB_OBJ_OP_BEGIN) ? job->job_id : p->object_id;
    obj_last.crc = crc;
    return MB_OK;
}

static void object_service(MbJobContext* job, uint32_t now) {
    (void)now;
    MbObjectState* s = job->module_state;
    if(s != NULL && s->performed) {
        job->done = true;
        job->done_status = s->op_status;
    }
}

static void object_request_stop(MbJobContext* job, MbStopReason reason) {
    (void)job;
    (void)reason;
}

static MbCleanupResult object_cleanup(MbJobContext* job) {
    (void)job;
    return MB_CLEAN_OK;
}

const MbModule mb_module_object = {
    .validate = object_validate_fn,
    .ctx_size = sizeof(MbObjectState),
    .start = object_start,
    .service = object_service,
    .request_stop = object_request_stop,
    .cleanup = object_cleanup,
};
