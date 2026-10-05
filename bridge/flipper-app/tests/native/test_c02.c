/*
 * test_c02.c - native qualification for core/bridge_diag (C02).
 * Ring wrap/overwrite accounting, snapshot coherence, paged read with gap
 * and has_more, epoch stamping, constant storage after >64 events.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../core/bridge_diag.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if(!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while(0)

int main(void) {
    /* 1. Record is exactly 32 bytes; storage is bounded. */
    CHECK(sizeof(MbTraceRecord) == 32);
    CHECK(sizeof(MbDiag) <= 64u * 32u + 64u);

    MbDiag d;
    mb_diag_init(&d);
    MbDiagSnapshot snap;
    mb_diag_snapshot(&d, &snap);
    CHECK(snap.count == 0 && snap.oldest_seq == 0 && snap.newest_seq == 0);
    CHECK(!snap.wrapped && snap.overwrites == 0 && snap.session_epoch == 0);

    /* 2. Fill exactly to capacity: no overwrites yet. */
    for(uint32_t i = 0; i < MB_TRACE_CAPACITY; i++) {
        mb_diag_push(&d, 1000 + i, MB_EV_APP_START, 7, 3, i, i * 2);
    }
    mb_diag_snapshot(&d, &snap);
    CHECK(snap.count == 64 && snap.oldest_seq == 1 && snap.newest_seq == 64);
    CHECK(snap.overwrites == 0 && !snap.wrapped);

    /* 3. Push past capacity: oldest advances, overwrites counted. */
    for(uint32_t i = 0; i < 136; i++) {
        mb_diag_push(&d, 2000 + i, MB_EV_FRAME_DROP, 9, 3, i, 0);
    }
    mb_diag_snapshot(&d, &snap);
    CHECK(snap.count == 64 && snap.oldest_seq == 137 && snap.newest_seq == 200);
    CHECK(snap.overwrites == 136 && snap.wrapped);

    /* 4. Paged read: from seq 0 returns the oldest available, gap flagged. */
    MbTraceRecord page[4];
    bool gap, more;
    uint32_t n = mb_diag_read_after(&d, 0, page, 4, &gap, &more);
    CHECK(n == 4 && gap && more);
    CHECK(page[0].trace_seq == 137 && page[3].trace_seq == 140);
    CHECK(page[0].event_code == MB_EV_FRAME_DROP && page[0].job_id == 9);

    /* 5. Continue paging to the end: last page short, has_more clears. */
    uint32_t cursor = page[3].trace_seq;
    uint32_t total = 4;
    while(1) {
        n = mb_diag_read_after(&d, cursor, page, 4, &gap, &more);
        if(n == 0) break;
        CHECK(!gap);
        for(uint32_t i = 0; i < n; i++) {
            CHECK(page[i].trace_seq == cursor + 1 + i);
        }
        cursor = page[n - 1].trace_seq;
        total += n;
        if(!more) break;
    }
    CHECK(total == 64 && cursor == 200);

    /* 6. Read-ahead of newest: nothing returned, gap flagged. */
    n = mb_diag_read_after(&d, 500, page, 4, &gap, &more);
    CHECK(n == 0 && gap && !more);
    n = mb_diag_read_after(&d, 200, page, 4, &gap, &more);
    CHECK(n == 0 && !gap && !more);

    /* 7. Session epoch increments and stamps new records. */
    mb_diag_new_session(&d);
    mb_diag_new_session(&d);
    mb_diag_push(&d, 3000, MB_EV_SESSION_LIVE, 11, 4, 0, 0);
    mb_diag_snapshot(&d, &snap);
    CHECK(snap.session_epoch == 2);
    n = mb_diag_read_after(&d, snap.newest_seq - 1, page, 4, &gap, &more);
    CHECK(n == 1 && page[0].session_epoch == 2);
    CHECK(page[0].event_code == MB_EV_SESSION_LIVE && page[0].tick == 3000);
    CHECK(page[0].connection_generation == 4 && page[0].job_id == 11);

    /* 8. Field fidelity through wrap: content survives verbatim. */
    MbDiag d2;
    mb_diag_init(&d2);
    mb_diag_push(&d2, 42, MB_EV_UART_ACQUIRED, 0x1122334455667788ULL, 5, 77, 88);
    n = mb_diag_read_after(&d2, 0, page, 4, &gap, &more);
    CHECK(n == 1 && !gap && !more);
    CHECK(page[0].trace_seq == 1 && page[0].tick == 42);
    CHECK(page[0].job_id == 0x1122334455667788ULL);
    CHECK(page[0].arg0 == 77 && page[0].arg1 == 88);

    if(failures == 0) {
        printf("PASS test_c02 (8 groups)\n");
        printf("sizeof(MbDiag)=%zu trace store=%zu bytes\n",
               sizeof(MbDiag), sizeof(((MbDiag*)0)->records));
        return 0;
    }
    printf("FAIL test_c02: %d check(s)\n", failures);
    return 1;
}
