#pragma once
/* Muse Bridge stable internal types (§4/§9).
 * Portable C11; the module lifecycle below is the project API every
 * hardware module implements (§4), faked by modules/module_fake.c
 * until real hardware modules qualify. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* §9.1 job lifecycle (executor-owned). */
typedef enum {
    MB_JOB_IDLE = 0,
    MB_JOB_ACCEPTED,
    MB_JOB_STARTING,
    MB_JOB_RUNNING,
    MB_JOB_STOPPING,
    MB_JOB_TERMINAL, /* terminal outcome recorded, awaiting controller ack */
    MB_JOB_FAULTED /* cleanup unproven; slot stays owned, never reused */
} MbJobState;

typedef enum {
    MB_STOP_NONE = 0,
    MB_STOP_DONE,
    MB_STOP_CANCEL,
    MB_STOP_TIMEOUT,
    MB_STOP_LINK,
    MB_STOP_LOCAL,
    MB_STOP_ERROR
} MbStopReason;

typedef enum { MB_CLEAN_OK, MB_CLEAN_FAILED } MbCleanupResult;

/* Allocator indirection so native tests can fail each allocation
 * boundary and verify unwind (§9.3). The FAP wires malloc/free. */
typedef struct {
    void* (*alloc)(void* ctx, size_t n);
    void (*free)(void* ctx, void* p);
    void* ctx;
} MbAlloc;

#define MB_DATA_PAYLOAD 32
typedef struct {
    uint32_t seq; /* counted at generation, gaps are visible (§8) */
    uint16_t len;
    uint8_t bytes[MB_DATA_PAYLOAD];
} MbDataRecord;

typedef struct MbJobContext MbJobContext;
struct MbJobContext {
    const MbAlloc* alloc;
    void* module_state;
    void* executor; /* opaque back-pointer for emit */
    uint64_t job_id;
    bool done; /* module sets this (with done_status) on natural finish */
    MbStatus done_status;
    bool (*emit)(MbJobContext* job, const uint8_t* data, uint16_t len);
};

typedef struct {
    MbStatus (*validate)(const void* params);
    size_t ctx_size;
    MbStatus (*start)(MbJobContext* job, const void* params);
    void (*service)(MbJobContext* job, uint32_t now);
    void (*request_stop)(MbJobContext* job, MbStopReason reason);
    MbCleanupResult (*cleanup)(MbJobContext* job);
} MbModule;

#ifdef __cplusplus
}
#endif
