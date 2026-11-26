// OhMyDSP daemon (root LaunchDaemon): bridge between the kext's user client and the AudioServerPlugIn. See ../protocol.h.
//   - opens the kext user client (retries until the kext has prepared the DSP; the kext starts it ~30 s after boot)
//   - on the first "start" from the driver: starts the stream, then copies new ring frames into the shared region every 2 ms
//   - on the last "stop" (or when the driver's connection dies): stops the stream, so the DSP never stays powered without a reader
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
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
#include <pthread.h>
#include <stdatomic.h>
#include <mach/thread_policy.h>
#include "../../include/OhMyDSPShared.h"
#include "../protocol.h"
#include "../diagnostics.h"
#include "../clock.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "ohmydspd: " fmt, ##__VA_ARGS__)

static dispatch_queue_t q;
static io_connect_t conn;
static const volatile struct OhMyDSPStatus *kst;
static const uint8_t *kring;
static struct OhMyDSPShm *shm; static uint8_t *sring; static size_t shmSize;
static uint32_t ringBytes, frameBytes;
static bool rplSession;
static uint32_t sessionPeriod(void)
{
    if (!rplSession) return ringBytes / frameBytes;
    return ringBytes / frameBytes == 131072u ? OMD_HAL_CLOCK_PERIOD : 0;
}
static int clients;
static pthread_t pumpThread;
static bool pumpJoinable;
static atomic_bool pumpQuit;
static kern_return_t pumpPolicyStatus;
static uint64_t pumpLateWakeups;
static uint32_t pos, lastEpoch; static uint64_t written;
static uint64_t legacySuspectSince;
static uint64_t diagnosticID, kernelSession;
static uint64_t diagPolls, diagCopied, diagInitial, diagEpochs, diagOverruns, diagSnapshotFailures;
static uint64_t diagLastPump, diagMaxGap, diagStart;
static double diagNsPerTick;
static struct OMDHalDiag diagClock;
static struct OMDClock deviceClock;
static uint64_t clockFailures;
static void diagDump(kern_return_t stopResult)
{
    char line[1024];
    snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"daemon\",\"id\":%llu,\"kernel_session\":%llu,\"epoch\":%u,\"polls\":%llu,\"copied\":%llu,\"initial_skipped\":%llu,\"written\":%llu,\"epoch_changes\":%llu,\"overruns\":%llu,\"snapshot_failures\":%llu,\"max_pump_gap_ms\":%.3f,\"elapsed_ms\":%.3f,\"stop_status\":%u,\"anchors\":%llu,\"max_anchor_residual_ns\":%.3f,\"events\":%u,\"dropped\":%u}",
        diagnosticID,kernelSession,lastEpoch,diagPolls,diagCopied,diagInitial,written,diagEpochs,diagOverruns,diagSnapshotFailures,diagMaxGap*diagNsPerTick/1e6,(mach_absolute_time()-diagStart)*diagNsPerTick/1e6,stopResult,diagClock.anchors,diagClock.maxAbsResidualNs,diagClock.count,diagClock.dropped);
    os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
    snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"pump\",\"id\":%llu,\"mode\":\"deadline_thread\",\"policy_status\":%u,\"period_us\":2000,\"budget_us\":200,\"constraint_us\":1000,\"late_wakeups\":%llu}",diagnosticID,pumpPolicyStatus,pumpLateWakeups);
    os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
#ifdef OHMYDSP_SMOOTH_CLOCK_EXPERIMENT
    if (rplSession) {
        snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"clock\",\"id\":%llu,\"contract\":%u,\"period\":%u,\"advances\":%llu,\"failures\":%llu,\"rejected\":%llu,\"rate_ppm\":%.6f,\"max_phase_ns\":%.3f}",diagnosticID,OMD_CLOCK_CONTRACT,OMD_CLOCK_PERIOD,deviceClock.advances,clockFailures,deviceClock.rejected,deviceClock.nominal?(deviceClock.step/deviceClock.nominal-1)*1e6:0,deviceClock.maxPhaseNs);
    } else
#endif
    {
        snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"clock\",\"id\":%llu,\"contract\":3,\"period\":%u,\"mode\":\"dma_lap\",\"profile\":\"%s\",\"failures\":%llu,\"rejected\":0}",
                 diagnosticID,sessionPeriod(),rplSession ? "ipc4" : "ipc3",clockFailures);
    }
    os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
    for(unsigned i=0;i<diagClock.count;i++) {
        const struct OMDHalEvent *e=&diagClock.events[i];
        snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"daemon_anchor\",\"id\":%llu,\"index\":%u,\"epoch\":%u,\"laps\":%llu,\"host\":%llu,\"hardware_frame\":%llu,\"state\":%u,\"residual_ns\":%.3f}",diagnosticID,i,e->p.epoch,e->p.laps,e->p.host,e->p.written,e->p.state,e->residualNs);
        os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
    }
}
static uid_t audioUid = (uid_t)-1;

static bool openKext(void)
{
    if (conn) return true;
    io_service_t svc = IOServiceGetMatchingService(0, IOServiceMatching(OHMYDSP_SERVICE_CLASS));
    if (!svc) return false;
    CFTypeRef bundle = IORegistryEntryCreateCFProperty(svc, CFSTR("CFBundleIdentifier"), kCFAllocatorDefault, 0);
    rplSession = bundle && CFEqual(bundle, CFSTR("com.ohmydsp.OhMyDSP.IPC4Session"));
    bool supported = rplSession || (bundle && CFEqual(bundle, CFSTR("com.ohmydsp.OhMyDSP")));
    if (bundle) CFRelease(bundle);
    if (!supported) { IOObjectRelease(svc); return false; }
    io_connect_t c = 0; kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c); IOObjectRelease(svc);
    if (kr) return false;      // not ready yet (DSP still being prepared) or already in use
    mach_vm_address_t ra = 0, sa = 0; mach_vm_size_t rs = 0, ss = 0;
    if (IOConnectMapMemory64(c, OHMYDSP_MEM_RING, mach_task_self(), &ra, &rs, kIOMapAnywhere | kIOMapReadOnly) ||
        IOConnectMapMemory64(c, OHMYDSP_MEM_STATUS, mach_task_self(), &sa, &ss, kIOMapAnywhere | kIOMapReadOnly)) { IOServiceClose(c); return false; }
    kst = (const volatile struct OhMyDSPStatus *)sa; kring = (const uint8_t *)ra;
    if (ss < sizeof(*kst) || kst->magic != OHMYDSP_STATUS_MAGIC || kst->version != OHMYDSP_STATUS_VERSION || kst->frameBytes != 8 || kst->channels != 2 || kst->rate != 48000 || kst->ringBytes < 16384 || kst->ringBytes > rs || kst->ringBytes % 8) { LOG("kext status magic/version mismatch"); IOServiceClose(c); return false; }
    ringBytes = kst->ringBytes; frameBytes = kst->frameBytes;
    if (!shm) {
        shmSize = OHMYDSP_SHM_HEADER + ringBytes;
        shm = mmap(NULL, shmSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
        if (shm == MAP_FAILED) { shm = NULL; IOServiceClose(c); return false; }
        sring = (uint8_t *)shm + OHMYDSP_SHM_HEADER;
        shm->magic = OHMYDSP_SHM_MAGIC; shm->version = OHMYDSP_SHM_VERSION; shm->clockPeriod=sessionPeriod(); shm->ringBytes = ringBytes; shm->frameBytes = frameBytes; shm->rate = kst->rate; shm->channels = kst->channels;
    }
    conn = c;
    LOG("kext user client open: ring %u bytes, rate %u", ringBytes, kst->rate);
    return true;
}

static void pumpOnce(void)
{
    uint64_t now=mach_absolute_time();diagPolls++;
    if(diagLastPump && now-diagLastPump>diagMaxGap)diagMaxGap=now-diagLastPump;
    diagLastPump=now;
    uint32_t lp=0,ep=0,state=0;uint64_t laps=0,ht=0;bool stable=false;
    for(unsigned attempt=0;attempt<OMD_SNAPSHOT_TRIES;attempt++) {
        uint32_t seq=kst->seq;if(seq&1)continue;__sync_synchronize();
        laps=kst->laps;ht=kst->lapHostTime;lp=kst->lpib;ep=kst->epoch;state=kst->state;
        __sync_synchronize();if(seq==kst->seq){stable=true;break;}
    }
    if(!stable){diagSnapshotFailures++;return;}
    lp %= ringBytes; lp -= lp % frameBytes;
    const uint64_t ringFrames = ringBytes / frameBytes;
    uint64_t expected = laps * ringFrames + lp / frameBytes;
    bool legacy = !rplSession && !(kst->capabilities & OHMYDSP_CAP_COHERENT_POSITION);
    if (legacy && state == OHMYDSP_STATE_SILENT) ep++;
    if (legacy && state == OHMYDSP_STATE_RUNNING) {
        // v1 IPC3 publishes the lap anchor before updating the byte position.
        // Even the first snapshot can contain the previous lap's tail.
        double elapsedNs = now >= ht ? (now - ht) * diagNsPerTick : 0;
        double positionNs = (lp / frameBytes) * (1e9 / 48000.0);
        bool mixed = laps && ht && positionNs > elapsedNs + 20000000.0;
        bool discontinuity = ep == lastEpoch &&
            (expected < written || expected - written >= ringFrames - 960);
        if (mixed || discontinuity) {
            if (!legacySuspectSince) legacySuspectSince = now;
            if ((now - legacySuspectSince) * diagNsPerTick < 20000000.0) return;
        }
        if (!mixed && !discontinuity) legacySuspectSince = 0;
    }
    struct OMDPosition observed={.epoch=ep,.state=state,.laps=laps,.host=ht,.written=expected};
    omdClock(&diagClock,&observed,(uint32_t)ringFrames,diagNsPerTick);
    if (ep != lastEpoch) {
        if(lastEpoch!=(uint32_t)-1)diagEpochs++;
        diagInitial+=expected;
        memset(sring, 0, ringBytes);
        lastEpoch = ep; pos = lp; written = expected;
    } else if (!rplSession && state == OHMYDSP_STATE_SILENT) {
        // No DMA producer in this epoch. The HAL renders zeros from the software timeline.
        pos = lp;
        written = expected;
    } else if (expected < written || expected - written >= ringFrames - 960) {
        diagOverruns++;
        LOG("capture overrun/discontinuity: expected %llu written %llu; stopping", expected, written);
        // Error cleanup is allowed to block; leave the realtime policy before IOKit stop.
        thread_port_t thread=mach_thread_self();
        thread_policy_set(thread,THREAD_STANDARD_POLICY,NULL,0);mach_port_deallocate(mach_task_self(),thread);
        IOConnectCallScalarMethod(conn, OHMYDSP_SEL_STOP, NULL, 0, NULL, NULL);
        atomic_store(&pumpQuit,true);
        memset(sring, 0, ringBytes);
        state = OHMYDSP_STATE_ERROR;
        written = 0;
    } else {
        while (pos != lp) {
            uint32_t end = lp > pos ? lp : ringBytes;
            memcpy(sring + pos, kring + pos, end - pos);
            diagCopied+=(end-pos)/frameBytes;
            written += (end - pos) / frameBytes; pos = end >= ringBytes ? 0 : end;
        }
    }
    bool clockOK = ht && (state == OHMYDSP_STATE_RUNNING ||
                           (!rplSession && state == OHMYDSP_STATE_SILENT));
#ifdef OHMYDSP_SMOOTH_CLOCK_EXPERIMENT
    if (rplSession) {
        clockOK = clockOK && omdClockUpdate(&deviceClock,ep,laps*ringFrames,ht,now,diagNsPerTick);
    } else
#endif
    {
        deviceClock.sample = laps * ringFrames;
        deviceClock.host = (double)ht;
    }
    if(state==OHMYDSP_STATE_RUNNING && !clockOK)clockFailures++;
    // Readers use the primary position while we fill the spare, then the spare
    // while we update the primary. Preemption of the pump leaves one readable.
    shm->nextPosition = (struct OMDPosition){ep,state,laps,ht,written,deviceClock.sample,(uint64_t)deviceClock.host,clockOK};
    shm->nextSeq = shm->seq + 1u;
    __sync_synchronize();
    shm->seq++; __sync_synchronize();
    shm->epoch = ep; shm->state = state; shm->laps = laps; shm->lapHostTime = ht; shm->written = written;
    shm->clockSample=deviceClock.sample;shm->clockHost=(uint64_t)deviceClock.host;shm->clockValid=clockOK;
    __sync_synchronize(); shm->seq++;
}

// Dedicated bounded-work pump: no XPC handling, allocations, logging or file IO in normal capture.
static void *pumpThreadMain(void *unused)
{
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    thread_time_constraint_policy_data_t policy={
        .period=(uint32_t)(2000000.0/diagNsPerTick),
        .computation=(uint32_t)(200000.0/diagNsPerTick),
        .constraint=(uint32_t)(1000000.0/diagNsPerTick),.preemptible=TRUE};
    thread_port_t thread=mach_thread_self();
    pumpPolicyStatus=thread_policy_set(thread,THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&policy,THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(),thread);
    uint64_t period=(uint64_t)(2000000.0/diagNsPerTick),next=mach_absolute_time()+period;
    while(!atomic_load_explicit(&pumpQuit,memory_order_acquire)) {
        mach_wait_until(next);
        if(atomic_load_explicit(&pumpQuit,memory_order_acquire))break;
        uint64_t now=mach_absolute_time();
        if(now>next && (now-next)*diagNsPerTick>1000000.0)pumpLateWakeups++;
        pumpOnce();
        next+=period;
        now=mach_absolute_time();
        if(next<=now)next=now+period; // never spin to catch up missed deadlines
    }
    return NULL;
}
static void stopPump(void)
{
    atomic_store_explicit(&pumpQuit,true,memory_order_release);
    if(pumpJoinable){pthread_join(pumpThread,NULL);pumpJoinable=false;}
}

static int64_t doStart(void)
{
    if (clients == 0) {
        if (!openKext()) return -1;
        if (!sessionPeriod()) return kIOReturnUnsupported;
        diagnosticID=diagStart=mach_absolute_time();kernelSession=0;
        diagPolls=diagCopied=diagInitial=diagEpochs=diagOverruns=diagSnapshotFailures=diagLastPump=diagMaxGap=0;
        memset(&diagClock,0,sizeof(diagClock));memset(&deviceClock,0,sizeof(deviceClock));clockFailures=0;
        kern_return_t kr = IOConnectCallScalarMethod(conn, OHMYDSP_SEL_START, NULL, 0, NULL, NULL);
        if (kr) { LOG("kext start failed 0x%x", kr); return kr; }
        // The connection's registry object may be the user client, not the OhMyDSP service.
        io_service_t svc=IOServiceGetMatchingService(0,IOServiceMatching(OHMYDSP_SERVICE_CLASS));
        if(svc) {
            CFTypeRef value=IORegistryEntryCreateCFProperty(svc,CFSTR("ipc4-session-number"),kCFAllocatorDefault,0);
            if(value){if(CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberSInt64Type,&kernelSession);CFRelease(value);}
            IOObjectRelease(svc);
        }
        legacySuspectSince = 0;
        lastEpoch = (uint32_t)-1; written = 0; pos = 0;
        pumpOnce();
        atomic_store(&pumpQuit,false);pumpPolicyStatus=KERN_FAILURE;pumpLateWakeups=0;
        if(pthread_create(&pumpThread,NULL,pumpThreadMain,NULL)!=0) {
            IOConnectCallScalarMethod(conn,OHMYDSP_SEL_STOP,NULL,0,NULL,NULL);
            shm->seq++;__sync_synchronize();shm->state=OHMYDSP_STATE_ERROR;shm->clockValid=0;
            __sync_synchronize();shm->seq++;return kIOReturnNoResources;
        }
        pumpJoinable=true;
        LOG("stream started");
    }
    clients++;
    return 0;
}

static void doStop(void)
{
    if (clients == 0) return;
    if (--clients == 0) {
        stopPump();
        kern_return_t kr = IOConnectCallScalarMethod(conn, OHMYDSP_SEL_STOP, NULL, 0, NULL, NULL);
        diagDump(kr);
        if (shm) {
            memset(sring, 0, ringBytes);
            shm->seq++; __sync_synchronize();
            shm->state = kr ? OHMYDSP_STATE_ERROR : OHMYDSP_STATE_STOPPED; shm->written = 0;shm->clockValid=0;
            __sync_synchronize(); shm->seq++;
        }
        LOG("stream stop result 0x%x", kr);
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
    mach_timebase_info_data_t tb;mach_timebase_info(&tb);diagNsPerTick=(double)tb.numer/tb.denom;
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
            if (cmd && !strcmp(cmd, "clock_info")) {
                uint32_t period = openKext() ? sessionPeriod() : 0;
                st = period ? 0 : kIOReturnNotReady;
                xpc_dictionary_set_uint64(r, "period", period);
            } else if (cmd && !strcmp(cmd, "start")) {
                st = started ? 0 : doStart();
                if (st == 0) {
                    started = true;
                    xpc_dictionary_set_uint64(r,"diagnostic_id",diagnosticID);
                    xpc_dictionary_set_uint64(r,"kernel_session",kernelSession);
                    xpc_object_t region = xpc_shmem_create(shm, shmSize);
                    xpc_dictionary_set_value(r, "shm", region);
                    xpc_release(region);
                    xpc_dictionary_set_uint64(r, "size", shmSize);
                }
            } else if (cmd && !strcmp(cmd, "stop")) { if (started) { started = false; doStop(); } st = 0; }
            xpc_dictionary_set_int64(r, "status", st);
            xpc_connection_send_message(p, r);
            xpc_release(r);
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
