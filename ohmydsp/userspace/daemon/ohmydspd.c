// OhMyDSP daemon (root LaunchDaemon): bridge between the kext's user client and the AudioServerPlugIn. See ../protocol.h.
//   - opens the kext user client (retries until the kext has prepared the DSP; the kext starts it ~30 s after boot)
//   - on the first "start" from the driver: starts the stream, then copies new ring frames into the shared region every 2 ms
//   - on the last "stop" (or when the driver's connection dies): stops the stream, so the DSP never stays powered without a reader
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <os/log.h>
#include "../../include/OhMyDSPShared.h"
#include "../protocol.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "ohmydspd: " fmt, ##__VA_ARGS__)

static dispatch_queue_t q;
static io_connect_t conn;
static const volatile struct OhMyDSPStatus *kst;
static const uint8_t *kring;
static struct OhMyDSPShm *shm; static uint8_t *sring; static size_t shmSize;
static uint32_t ringBytes, frameBytes;
static int clients;
static dispatch_source_t pump;
static uint32_t pos, lastEpoch; static uint64_t written;
static uid_t audioUid = (uid_t)-1;

static bool openKext(void)
{
    if (conn) return true;
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(OHMYDSP_SERVICE_CLASS));
    if (!svc) return false;
    io_connect_t c = 0; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c); IOObjectRelease(svc);
    if (kr) return false;      // not ready yet (DSP still being prepared) or already in use
    mach_vm_address_t ra = 0, sa = 0; mach_vm_size_t rs = 0, ss = 0;
    if (IOConnectMapMemory64(c, OHMYDSP_MEM_RING, mach_task_self(), &ra, &rs, kIOMapAnywhere | kIOMapReadOnly) ||
        IOConnectMapMemory64(c, OHMYDSP_MEM_STATUS, mach_task_self(), &sa, &ss, kIOMapAnywhere | kIOMapReadOnly)) { IOServiceClose(c); return false; }
    kst = (const volatile struct OhMyDSPStatus *)sa; kring = (const uint8_t *)ra;
    if (kst->magic != OHMYDSP_STATUS_MAGIC || kst->version != OHMYDSP_STATUS_VERSION) { LOG("kext status magic/version mismatch"); IOServiceClose(c); return false; }
    ringBytes = kst->ringBytes; frameBytes = kst->frameBytes;
    if (!shm) {
        shmSize = OHMYDSP_SHM_HEADER + ringBytes;
        shm = mmap(NULL, shmSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        if (shm == MAP_FAILED) { shm = NULL; IOServiceClose(c); return false; }
        sring = (uint8_t *)shm + OHMYDSP_SHM_HEADER;
        shm->magic = OHMYDSP_SHM_MAGIC; shm->version = 1; shm->ringBytes = ringBytes; shm->frameBytes = frameBytes; shm->rate = kst->rate; shm->channels = kst->channels;
    }
    conn = c;
    LOG("kext user client open: ring %u bytes, rate %u", ringBytes, kst->rate);
    return true;
}

static void pumpOnce(void)
{
    uint32_t s1, s2, lp, ep, state; uint64_t laps, ht;
    do { s1 = kst->seq; laps = kst->laps; ht = kst->lapHostTime; lp = kst->lpib; ep = kst->epoch; state = kst->state; s2 = kst->seq; } while ((s1 & 1) || s1 != s2);
    lp %= ringBytes; lp -= lp % frameBytes;
    const uint64_t ringFrames = ringBytes / frameBytes;
    uint64_t expected = laps * ringFrames + lp / frameBytes;
    if (ep != lastEpoch || expected > written + ringFrames - 960 || expected + ringFrames < written) {   // restart, or we fell a lap behind: resync
        lastEpoch = ep; pos = lp; written = expected;
    } else {
        while (pos != lp) {
            uint32_t end = lp > pos ? lp : ringBytes;
            memcpy(sring + pos, kring + pos, end - pos);
            written += (end - pos) / frameBytes; pos = end >= ringBytes ? 0 : end;
        }
    }
    __sync_synchronize();
    shm->seq++; __sync_synchronize();
    shm->epoch = ep; shm->state = state; shm->laps = laps; shm->lapHostTime = ht; shm->written = written;
    __sync_synchronize(); shm->seq++;
}

static int64_t doStart(void)
{
    if (clients == 0) {
        if (!openKext()) return -1;
        kern_return_t kr = IOConnectCallScalarMethod(conn, OHMYDSP_SEL_START, NULL, 0, NULL, NULL);
        if (kr) { LOG("kext start failed 0x%x", kr); return kr; }
        lastEpoch = (uint32_t)-1; written = 0; pos = 0;
        pumpOnce();
        pump = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(pump, dispatch_time(DISPATCH_TIME_NOW, 0), 2 * NSEC_PER_MSEC, 200 * NSEC_PER_USEC);
        dispatch_source_set_event_handler(pump, ^{ pumpOnce(); });
        dispatch_resume(pump);
        LOG("stream started");
    }
    clients++;
    return 0;
}

static void doStop(void)
{
    if (clients == 0) return;
    if (--clients == 0) {
        dispatch_source_cancel(pump); pump = NULL;
        IOConnectCallScalarMethod(conn, OHMYDSP_SEL_STOP, NULL, 0, NULL, NULL);
        LOG("stream stopped");
    }
}

static bool peerAllowed(xpc_connection_t peer)
{
    // Only coreaudiod's driver host may read the microphone through this daemon (TCC is enforced by CoreAudio, not here).
    uid_t u = xpc_connection_get_euid(peer);
    return u == audioUid || u == 0;
}

int main(void)
{
    struct passwd *pw = getpwnam("_coreaudiod"); if (pw) audioUid = pw->pw_uid;
    q = dispatch_queue_create("com.ohmydsp.daemon", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    xpc_connection_t l = xpc_connection_create_mach_service(OHMYDSP_MACH_SERVICE, q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
        if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) return;
        xpc_connection_t p = (xpc_connection_t)peer;
        if (!peerAllowed(p)) { LOG("refused connection from uid %u", xpc_connection_get_euid(p)); xpc_connection_cancel(p); return; }
        __block bool started = false;
        xpc_connection_set_target_queue(p, q);
        xpc_connection_set_event_handler(p, ^(xpc_object_t msg) {
            if (xpc_get_type(msg) == XPC_TYPE_ERROR) { if (started) { started = false; doStop(); LOG("driver connection lost, stream stopped"); } return; }
            if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) return;
            xpc_object_t r = xpc_dictionary_create_reply(msg); if (!r) return;
            const char *cmd = xpc_dictionary_get_string(msg, "cmd");
            int64_t st = -2;
            if (cmd && !strcmp(cmd, "start")) {
                st = started ? 0 : doStart();
                if (st == 0) {
                    started = true;
                    xpc_dictionary_set_value(r, "shm", xpc_shmem_create(shm, shmSize));
                    xpc_dictionary_set_uint64(r, "size", shmSize);
                }
            } else if (cmd && !strcmp(cmd, "stop")) { if (started) { started = false; doStop(); } st = 0; }
            xpc_dictionary_set_int64(r, "status", st);
            xpc_connection_send_message(p, r);
        });
        xpc_connection_resume(p);
    });
    xpc_connection_resume(l);
    // keep trying to open the kext so the first "start" is fast and the log shows when it became available
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    dispatch_source_set_timer(t, DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(t, ^{ if (!conn && clients == 0) openKext(); });
    dispatch_resume(t);
    LOG("started (coreaudiod uid %d)", (int)audioUid);
    dispatch_main();
}
