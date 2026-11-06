// Phase 6.1c: acts like the AudioServerPlugIn towards ohmydspd (start over XPC, map the shared region, read frames, stop) but runs as root
// outside coreaudiod, so it checks the daemon + XPC shared memory path alone.
//   cc -O2 -fblocks -o local/daemontest tools/daemontest.c && sudo local/daemontest [seconds=4]
#include <xpc/xpc.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../ohmydsp/userspace/protocol.h"

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 4;
    xpc_connection_t c = xpc_connection_create_mach_service(OHMYDSP_MACH_SERVICE, dispatch_get_global_queue(0, 0), 0);
    xpc_connection_set_event_handler(c, ^(xpc_object_t e) { (void)e; });
    xpc_connection_resume(c);
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0); xpc_dictionary_set_string(m, "cmd", "start");
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t a = mach_absolute_time();
    xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, m);
    printf("start reply in %.0f ms: ", (double)(mach_absolute_time() - a) * tb.numer / tb.denom / 1e6);
    if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) { printf("error: %s\n", xpc_dictionary_get_string(r, XPC_ERROR_KEY_DESCRIPTION)); return 1; }
    printf("status %lld\n", (long long)xpc_dictionary_get_int64(r, "status"));
    void *region = NULL; size_t sz = xpc_shmem_map(xpc_dictionary_get_value(r, "shm"), &region);
    if (!sz) { printf("no shared region\n"); return 1; }
    const volatile struct OhMyDSPShm *h = region; const int32_t *ring = (const int32_t *)((const uint8_t *)region + OHMYDSP_SHM_HEADER);
    printf("shm %zu bytes, magic 0x%08x ring %u bytes rate %u\n", sz, h->magic, h->ringBytes, h->rate);
    const int64_t rf = h->ringBytes / h->frameBytes;
    int64_t next = h->written; uint64_t nzA = 0, nzB = 0, total = 0; int32_t pkA = 0, pkB = 0; uint32_t ep0 = h->epoch; uint64_t lastLaps = h->laps; unsigned laps = 0;
    uint64_t t0 = mach_absolute_time();
    while ((double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9 < secs) {
        usleep(10000);
        int64_t w = h->written;
        if (w - next > rf - 480) next = w - rf + 480;               // too slow: skip like the driver would
        for (; next < w; next++, total++) {
            int32_t x = ring[2 * (next % rf)], y = ring[2 * (next % rf) + 1];
            nzA += x != 0; nzB += y != 0; if (abs(x) > pkA) pkA = abs(x); if (abs(y) > pkB) pkB = abs(y);
        }
        if (h->laps != lastLaps) { lastLaps = h->laps; laps++; }
    }
    double el = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9;
    printf("read %llu frames in %.2f s (expected ~%.0f), laps seen %u, epoch %u -> %u, state %u\n", total, el, el * h->rate, laps, ep0, h->epoch, h->state);
    printf("A: non-zero %llu peak %.1f dBFS | B: non-zero %llu peak %.1f dBFS\n", nzA, pkA ? 20 * log10(pkA / 2147483648.0) : -200.0, nzB, pkB ? 20 * log10(pkB / 2147483648.0) : -200.0);
    xpc_object_t m2 = xpc_dictionary_create(NULL, NULL, 0); xpc_dictionary_set_string(m2, "cmd", "stop");
    xpc_object_t r2 = xpc_connection_send_message_with_reply_sync(c, m2);
    printf("stop status %lld\n", (long long)xpc_dictionary_get_int64(r2, "status"));
    return 0;
}
