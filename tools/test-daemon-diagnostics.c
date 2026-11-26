// Exercise the real pump with in-memory kernel status/rings; the only stop call is mocked.
#define mach_absolute_time testMachTime
#define main daemon_main_unused
#define IOServiceGetMatchingService testMatchingService
#define IOConnectCallScalarMethod testScalarMethod
#include "../ohmydsp/userspace/daemon/ohmydspd.c"
#undef main
#undef mach_absolute_time
#undef IOConnectCallScalarMethod
#include <assert.h>
static unsigned starts, stops;
io_service_t testMatchingService(mach_port_t port, CFDictionaryRef matching)
{
    (void)port;
    CFRelease(matching);
    return IO_OBJECT_NULL;
}
static uint64_t mockNow=1000000000ULL;
uint64_t testMachTime(void){return mockNow;}
kern_return_t testScalarMethod(mach_port_t c,uint32_t selector,const uint64_t *in,uint32_t n,uint64_t *out,uint32_t *outN)
{(void)c;(void)in;(void)n;(void)out;(void)outN;if(selector==OHMYDSP_SEL_START)starts++;else {assert(selector==OHMYDSP_SEL_STOP);stops++;}return 0;}
static void exerciseStart(uint32_t bytes, bool rpl, bool legacy)
{
    struct OhMyDSPStatus status = {.capabilities=OHMYDSP_CAP_COHERENT_POSITION, .state=OHMYDSP_STATE_RUNNING, .epoch=1, .lapHostTime=mockNow};
    struct OhMyDSPShm shared = {0};
    if (legacy) status.capabilities = 0;
    conn = 1; kst = &status; shm = &shared;
    ringBytes = bytes; frameBytes = 8; rplSession = rpl; diagNsPerTick = 1;
    kring = calloc(1, bytes); sring = calloc(1, bytes);
    assert(kring && sring);
    const uint32_t frames = bytes / frameBytes;
    shared.clockPeriod = sessionPeriod();
    assert(shared.clockPeriod == frames);
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        unsigned previous = starts;
        status.state = OHMYDSP_STATE_RUNNING;
        status.epoch++;
        status.lpib = legacy ? bytes - 128 : 800;
        status.laps = legacy ? 1 : 0;
        status.lapHostTime = mockNow;

        assert(doStart() == 0 && starts == previous + 1 && (legacy || shared.clockValid));
        stopPump(); // Advance deterministic status snapshots on this thread.
        if (legacy) {
            assert(lastEpoch == UINT32_MAX); // START sampled the old tail and new anchor.
            status.lpib = 80;
            mockNow += 2000000;
            pumpOnce();
            assert(written == frames + 10 && shared.clockValid);
            status.lpib = bytes - 1024 * 8;
            mockNow = status.lapHostTime + (frames-1024)*1000000000ULL/48000;
            pumpOnce();
            status.lpib = bytes - 128;
            mockNow = status.lapHostTime + (frames-16)*1000000000ULL/48000;
            pumpOnce();
            uint64_t before = diagCopied;
            status.laps++;
            status.lapHostTime = mockNow += 1000000;
            pumpOnce();
            assert(diagCopied == before && !diagOverruns);
            status.lpib = 8;
            pumpOnce();
            assert(diagCopied == before + 17 && written == frames*2 + 1);
            if (cycle) {
                status.laps += 2;
                status.lapHostTime = mockNow += frames * 2000000000ULL / 48000;
                pumpOnce();
                mockNow += 22000000;
                status.lpib += 96 * 8;
                pumpOnce();
                assert(diagOverruns == 1 && shared.state == OHMYDSP_STATE_ERROR);
                doStop();
                continue;
            }
            memset(sring, 0x6a, bytes);
        }
        if (!rpl) {
            status.state = OHMYDSP_STATE_SILENT;
            if (!legacy) status.epoch++;
            status.lpib = 0;
            for (unsigned lap = 0; lap <= 2; lap++) {
                status.laps = lap;
                status.lapHostTime += frames * 1000000000ULL / 48000;
                mockNow = status.lapHostTime;
                pumpOnce();
                assert(shared.clockValid && shared.state == OHMYDSP_STATE_SILENT);
                assert(shared.written == (uint64_t)lap * frames && shared.clockSample == shared.written);
                assert(shared.clockHost == mockNow && shared.epoch == status.epoch + legacy && !diagOverruns);
            }
        }
        if (legacy) for (unsigned i = 0; i < bytes; i++) assert(!sring[i]);
        doStop();
        assert(!clients && !shared.clockValid && shared.state == OHMYDSP_STATE_STOPPED);
    }
    if (rpl) {
        ringBytes /= 2;
        unsigned previous = starts;
        assert(doStart() == kIOReturnUnsupported && starts == previous);
    }
    free((void *)kring); free(sring);
}
int main(void) {
    struct OhMyDSPStatus status={.capabilities=OHMYDSP_CAP_COHERENT_POSITION};struct OhMyDSPShm shared={0};
    ringBytes=1048576;frameBytes=8;diagNsPerTick=1;lastEpoch=(uint32_t)-1;
    uint8_t *input=malloc(ringBytes),*output=calloc(1,ringBytes);assert(input&&output);
    memset(input,0x6a,ringBytes);kst=&status;kring=input;shm=&shared;sring=output;
    status.state=1;status.epoch=1;status.lpib=131000*8;status.lapHostTime=1000000000ULL;mockNow=status.lapHostTime+(uint64_t)(131000*(1e9/48000.0));
    pumpOnce();assert(shared.clockValid && shared.clockSample%512==0);assert(diagInitial==131000&&diagCopied==0&&written==131000&&shared.written==131000);
    status.laps=1;status.lpib=100*8;status.lapHostTime+=2730666667ULL;mockNow=status.lapHostTime+2083334;
    pumpOnce();assert(diagCopied==172&&written==131172&&diagClock.anchors==2);
    assert(shared.nextSeq==shared.seq-1 && shared.nextPosition.written==shared.written);
    assert(shared.nextPosition.state==shared.state && shared.nextPosition.epoch==shared.epoch);
    assert(shared.nextPosition.clockSample==shared.clockSample && shared.nextPosition.clockHost==shared.clockHost);
    assert(output[131000*8]==0x6a&&output[99*8]==0x6a&&output[100*8]==0);
    status.seq=1;pumpOnce();assert(diagSnapshotFailures==1&&written==131172);status.seq=2;
    status.epoch=2;status.laps=0;status.lpib=80;status.lapHostTime=mockNow;pumpOnce();assert(diagEpochs==1&&written==10&&diagInitial==131010);
    status.lpib=ringBytes-1000*8;pumpOnce();assert(diagOverruns==0);
    status.laps+=2;pumpOnce();assert(diagOverruns==1&&stops==1&&shared.state==3&&shared.written==0);
    free(input);free(output);
    exerciseStart(128*1024, false, false);
    exerciseStart(128*1024, false, true);
    exerciseStart(256*1024, false, false);
    exerciseStart(256*1024, false, true);
    exerciseStart(1024*1024, true, false);
    puts("Daemon actual-pump diagnostics: initial skipped/copy/wrap accounting, bounded snapshot, epoch change and overrun PASS");
}
