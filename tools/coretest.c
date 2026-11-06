// Phase 6.1 test client for the OhMyDSP core kext (no audio stack): opens the user client (root), maps the ring and the status page,
// starts the stream, copies the new frames out of the ring every few ms for N seconds, stops, prints statistics and optionally writes a WAV.
//   cc -O2 -o local/coretest tools/coretest.c -framework IOKit -framework CoreFoundation
//   sudo local/coretest [seconds=5] [out.wav] [inject=1|2]
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "../ohmydsp/include/OhMyDSPShared.h"

static void wavhdr(FILE *f, uint32_t bytes, uint32_t rate, uint16_t ch) {
    uint32_t v32; uint16_t v16;
    fwrite("RIFF", 1, 4, f); v32 = 36 + bytes; fwrite(&v32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v32 = 16; fwrite(&v32, 4, 1, f); v16 = 1; fwrite(&v16, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    v32 = rate * ch * 4; fwrite(&v32, 4, 1, f); v16 = ch * 4; fwrite(&v16, 2, 1, f); v16 = 32; fwrite(&v16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 5; const char *out = argc > 2 ? argv[2] : NULL; int inject = argc > 3 ? atoi(argv[3]) : 0;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(OHMYDSP_SERVICE_CLASS));
    if (!svc) { fprintf(stderr, "no OhMyDSP service (kext not loaded?)\n"); return 1; }
    io_connect_t c; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c);
    if (kr) { fprintf(stderr, "IOServiceOpen failed 0x%x (not root, DSP not ready yet, or another client is connected)\n", kr); return 1; }
    mach_vm_address_t ra = 0, sa = 0; mach_vm_size_t rs = 0, ss = 0;
    if ((kr = IOConnectMapMemory64(c, OHMYDSP_MEM_RING, mach_task_self(), &ra, &rs, kIOMapAnywhere | kIOMapReadOnly))) { fprintf(stderr, "map ring failed 0x%x\n", kr); return 1; }
    if ((kr = IOConnectMapMemory64(c, OHMYDSP_MEM_STATUS, mach_task_self(), &sa, &ss, kIOMapAnywhere | kIOMapReadOnly))) { fprintf(stderr, "map status failed 0x%x\n", kr); return 1; }
    volatile struct OhMyDSPStatus *st = (volatile struct OhMyDSPStatus *)sa; const uint8_t *ring = (const uint8_t *)ra;
    printf("mapped ring %llu bytes, status %llu bytes; magic 0x%08x version %u ringBytes %u frameBytes %u rate %u ch %u\n",
           rs, ss, st->magic, st->version, st->ringBytes, st->frameBytes, st->rate, st->channels);
    if (st->magic != OHMYDSP_STATUS_MAGIC) { fprintf(stderr, "bad magic\n"); return 1; }
    const uint32_t RB = st->ringBytes, FB = st->frameBytes;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    uint64_t t_start = mach_absolute_time();
    kr = IOConnectCallScalarMethod(c, OHMYDSP_SEL_START, NULL, 0, NULL, NULL);
    double start_ms = (double)(mach_absolute_time() - t_start) * tb.numer / tb.denom / 1e6;
    printf("start -> 0x%x in %.0f ms, state %u\n", kr, start_ms, st->state);
    if (kr) { IOServiceClose(c); return 1; }
    size_t cap = (size_t)(secs * st->rate * FB) + 16 * RB; uint8_t *buf = malloc(cap); size_t n = 0;
    uint32_t pos = st->lpib; uint64_t laps0 = st->laps, lastLaps = laps0, lapTimeLast = 0, lapMin = ~0ULL, lapMax = 0; unsigned lapCount = 0;
    uint64_t t0 = mach_absolute_time(); int injected = 0;
    while ((double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9 < secs) {
        usleep(3000);
        uint32_t seq1, seq2; uint64_t laps, ht; uint32_t lp, state;
        do { seq1 = st->seq; laps = st->laps; ht = st->lapHostTime; lp = st->lpib; state = st->state; seq2 = st->seq; } while ((seq1 & 1) || seq1 != seq2);
        if (inject && !injected && (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9 > secs / 3) {
            uint64_t in[2] = { OHMYDSP_TUNE_INJECT, (uint64_t)inject };
            printf("inject %d -> 0x%x\n", inject, IOConnectCallScalarMethod(c, OHMYDSP_SEL_TUNE, in, 2, NULL, NULL)); injected = 1;
        }
        if (laps != lastLaps) {
            if (lapTimeLast) { uint64_t d = (ht - lapTimeLast) * tb.numer / tb.denom; if (d < lapMin) lapMin = d; if (d > lapMax) lapMax = d; lapCount++; }
            lapTimeLast = ht; lastLaps = laps;
        }
        lp %= RB; lp -= lp % FB;
        while (pos != lp && n + FB <= cap) { memcpy(buf + n, ring + pos, FB); n += FB; pos += FB; if (pos >= RB) pos = 0; }
        if (state != OHMYDSP_STATE_RUNNING) { printf("state %u (not running) at %.1f s\n", state, (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e9); if (state == OHMYDSP_STATE_SILENT) break; }
    }
    uint64_t t_end = mach_absolute_time();
    uint32_t fl = st->state; uint64_t faults = st->faults, rec = st->recoveries, laps = st->laps;
    kr = IOConnectCallScalarMethod(c, OHMYDSP_SEL_STOP, NULL, 0, NULL, NULL);
    printf("stop -> 0x%x, state %u -> %u\n", kr, fl, st->state);
    double el = (double)(t_end - t0) * tb.numer / tb.denom / 1e9; size_t frames = n / FB;
    uint64_t nzA = 0, nzB = 0; int32_t pkA = 0, pkB = 0; const int32_t *s = (const int32_t *)buf;
    for (size_t i = 0; i < frames; i++) { int32_t a = s[2*i], b = s[2*i+1]; nzA += a != 0; nzB += b != 0; if (abs(a) > pkA) pkA = abs(a); if (abs(b) > pkB) pkB = abs(b); }
    printf("elapsed %.2f s, copied %zu frames (expected ~%.0f), laps %llu (+%llu), faults %llu recoveries %llu\n", el, frames, el * st->rate, laps, laps - laps0, faults, rec);
    printf("lap period over %u laps: min %.1f ms max %.1f ms (ring = %.1f ms)\n", lapCount, lapMin / 1e6, lapMax / 1e6, 1000.0 * (RB / FB) / st->rate);
    printf("A: non-zero %llu peak %.1f dBFS | B: non-zero %llu peak %.1f dBFS\n", nzA, pkA ? 20 * log10(pkA / 2147483648.0) : -200.0, nzB, pkB ? 20 * log10(pkB / 2147483648.0) : -200.0);
    if (out) { FILE *f = fopen(out, "wb"); if (f) { wavhdr(f, (uint32_t)n, st->rate, 2); fwrite(buf, 1, n, f); fclose(f); printf("wrote %s (raw S32 2 ch)\n", out); } }
    IOServiceClose(c);
    return 0;
}
