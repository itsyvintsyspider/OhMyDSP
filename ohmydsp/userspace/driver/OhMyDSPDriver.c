// OhMyDSP AudioServerPlugIn: publishes "Internal Microphone" (input only, 48 kHz, 2 ch Float32) and reads the DMIC ring that the root
// daemon (ohmydspd) shares over XPC. Structure follows Apple's NullAudio sample. Runs inside coreaudiod's sandbox: no IOKit, only the Mach
// service declared in Info.plist (AudioServerPlugIn_MachServices).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <xpc/xpc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <os/log.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "../diagnostics.h"
#include "../clock.h"
#include "../protocol.h"
#include "../../include/OhMyDSPShared.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "OhMyDSP driver: " fmt, ##__VA_ARGS__)

enum { kObjPlugIn = kAudioObjectPlugInObject, kObjDevice = 2, kObjStream = 3, kObjVolume = 4, kObjMute = 5, kObjSource = 6 };
enum { kSourceID = 'imic' };
enum { kRate = 48000, kChannels = 2, kSafetyOffset = 960 };
// Input volume: -28 .. +22 dB, scalar 0..1 maps linearly onto it; default +9 dB (as the IOAudio engine had). Persisted through the HAL's storage.
static const Float32 kDbMin = -28.0f, kDbMax = 22.0f, kDbDefault = 9.0f;
static Float32 gGainDb = 9.0f;
static volatile float gGainLin = 2.8183829f;
static volatile UInt32 gMute;
static void setGainDb(Float32 db) { if (db < kDbMin) db = kDbMin; if (db > kDbMax) db = kDbMax; gGainDb = db; gGainLin = powf(10.0f, db / 20.0f); }

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gIO;                                   // StartIO count
static xpc_connection_t gConn;
static const volatile struct OhMyDSPShm *gShm; static const uint8_t *gRing; static size_t gShmSize; static uint32_t gRingFrames;

// Diagnostic recording never waits or logs on the audio/clock callbacks. Contention is counted.
static struct OMDHalDiag gDiag;
static atomic_flag gDiagLock=ATOMIC_FLAG_INIT;
static atomic_uint_fast64_t gDiagSkipped;
static atomic_bool gDiagEnabled;
static uint64_t gDiagID, gKernelSession;
static double gNsPerTick;
static bool diagEnter(void) {
    if(!atomic_load_explicit(&gDiagEnabled,memory_order_relaxed))return false;
    if(!atomic_flag_test_and_set_explicit(&gDiagLock,memory_order_acquire)) {
        if(atomic_load_explicit(&gDiagEnabled,memory_order_relaxed))return true;
        atomic_flag_clear_explicit(&gDiagLock,memory_order_release);return false;
    }
    atomic_fetch_add_explicit(&gDiagSkipped,1,memory_order_relaxed);return false;
}
static void diagLeave(void){atomic_flag_clear_explicit(&gDiagLock,memory_order_release);}
static void diagDump(void)
{
    // StopIO is outside the realtime callbacks; take a bounded snapshot, then format/log it.
    struct OMDHalDiag copy; bool got=false;
    for(unsigned i=0;i<100;i++) {
        if(!atomic_flag_test_and_set_explicit(&gDiagLock,memory_order_acquire)){copy=gDiag;diagLeave();got=true;break;}
        usleep(100);
    }
    char line[1024];
    if(!got){LOG("diagnostic snapshot unavailable at stop");return;}
    snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"hal\",\"id\":%llu,\"kernel_session\":%llu,\"epoch\":%u,\"reads\":%llu,\"requested\":%llu,\"negative\":%llu,\"future\":%llu,\"stale\":%llu,\"state_frames\":%llu,\"snapshot_frames\":%llu,\"steady_missing\":%llu,\"clock_calls\":%llu,\"clock_snapshot_failures\":%llu,\"anchors\":%llu,\"clock_regressions\":%llu,\"max_anchor_residual_ns\":%.3f,\"events\":%u,\"dropped\":%u,\"skipped\":%llu}",
        gDiagID,gKernelSession,copy.previous.epoch,copy.reads,copy.requested,copy.negative,copy.future,copy.stale,copy.stateFrames,copy.snapshotFrames,copy.steadyMissing,copy.clockCalls,copy.clockSnapshotFailures,copy.anchors,copy.clockRegressions,copy.maxAbsResidualNs,copy.count,copy.dropped,(uint64_t)atomic_load(&gDiagSkipped));
    os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
    for(unsigned i=0;i<copy.count;i++) {
        const struct OMDHalEvent *e=&copy.events[i];
        snprintf(line,sizeof(line),"{\"v\":1,\"source\":\"hal_event\",\"id\":%llu,\"index\":%u,\"kind\":%u,\"epoch\":%u,\"state\":%u,\"first\":%lld,\"frames\":%u,\"written\":%llu,\"laps\":%llu,\"host\":%llu,\"negative\":%llu,\"future\":%llu,\"stale\":%llu,\"residual_ns\":%.3f}",gDiagID,i,e->kind,e->p.epoch,e->p.state,e->first,e->frames,e->p.written,e->p.laps,e->p.host,e->negative,e->future,e->stale,e->residualNs);
        os_log(OS_LOG_DEFAULT,"OhMyDSP DIAG %{public}s",line);
    }
}

// ---------------------------------------------------------------- daemon link
static atomic_uint gClockPeriod;

static xpc_connection_t daemonConnection(void)
{
    xpc_connection_t c = xpc_connection_create_mach_service(OHMYDSP_MACH_SERVICE,
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), 0);
    if (c) {
        xpc_connection_set_event_handler(c, ^(xpc_object_t event) { (void)event; });
        xpc_connection_resume(c);
    }
    return c;
}

// Discover the period before advertising the device, including after delayed kext preparation.
static void discoverClock(void)
{
    xpc_connection_t c = daemonConnection();
    uint64_t period = 0;
    if (c) {
        xpc_object_t query = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(query, "cmd", "clock_info");
        xpc_object_t reply = xpc_connection_send_message_with_reply_sync(c, query);
        if (reply && xpc_get_type(reply) == XPC_TYPE_DICTIONARY &&
            xpc_dictionary_get_int64(reply, "status") == 0)
            period = xpc_dictionary_get_uint64(reply, "period");
        if (reply) xpc_release(reply);
        xpc_release(query);
        xpc_connection_cancel(c);
        xpc_release(c);
    }
    if (!period || period > UINT32_MAX) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ discoverClock(); });
        return;
    }
    atomic_store(&gClockPeriod, (UInt32)period);
    AudioObjectPropertyAddress changed[] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}
    };
    gHost->PropertiesChanged(gHost, kObjPlugIn, 2, changed);
}

static bool linkStart(void)
{
    if (!atomic_load(&gClockPeriod)) return false;
    gConn = daemonConnection();
    if (!gConn) return false;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0); xpc_dictionary_set_string(m, "cmd", "start");
    xpc_object_t r = xpc_connection_send_message_with_reply_sync(gConn, m);
    bool ok = false;
    if (xpc_get_type(r) == XPC_TYPE_DICTIONARY && xpc_dictionary_get_int64(r, "status") == 0) {
        xpc_object_t sh = xpc_dictionary_get_value(r, "shm");
        void *region = NULL; size_t sz = sh ? xpc_shmem_map(sh, &region) : 0;
        if (sz >= OHMYDSP_SHM_HEADER && region) {
            const volatile struct OhMyDSPShm *h = region;
            if (h->magic == OHMYDSP_SHM_MAGIC && h->version == OHMYDSP_SHM_VERSION && h->clockPeriod == atomic_load(&gClockPeriod) && h->rate==kRate && h->ringBytes>=16384 && h->ringBytes%8==0 && h->channels == 2 && h->frameBytes == 8 && sz >= OHMYDSP_SHM_HEADER + h->ringBytes) {
                gDiagID=xpc_dictionary_get_uint64(r,"diagnostic_id"); gKernelSession=xpc_dictionary_get_uint64(r,"kernel_session");
                gShm = h; gRing = (const uint8_t *)region + OHMYDSP_SHM_HEADER; gShmSize = sz; gRingFrames = h->ringBytes / h->frameBytes; ok = true;
            } else { vm_deallocate(mach_task_self(),(vm_address_t)region,sz); LOG("shared region has a bad header/version"); }
        } else LOG("no shared region in the reply (size %zu)", sz);
    } else if (xpc_get_type(r) == XPC_TYPE_ERROR) LOG("daemon unreachable: %s", xpc_dictionary_get_string(r, XPC_ERROR_KEY_DESCRIPTION));
    else LOG("daemon refused the start (status %lld)", (long long)xpc_dictionary_get_int64(r, "status"));
    xpc_release(m);
    xpc_release(r);
    if (!ok) { xpc_connection_cancel(gConn); xpc_release(gConn); gConn = NULL; }
    return ok;
}

static void linkStop(void)
{
    if (!gConn) return;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0); xpc_dictionary_set_string(m, "cmd", "stop");
    xpc_object_t r = xpc_connection_send_message_with_reply_sync(gConn, m);
    xpc_release(r);
    xpc_release(m);
    xpc_connection_cancel(gConn); xpc_release(gConn); gConn = NULL;
    if (gShm) { vm_deallocate(mach_task_self(), (vm_address_t)gShm, gShmSize); gShm = NULL; gRing = NULL; }
}

// ---------------------------------------------------------------- properties
#define UNKNOWN kAudioHardwareUnknownPropertyError
typedef struct { UInt32 in; UInt32 *outSize; void *out; Boolean sizeOnly; UInt32 qSize; const void *qData; } Ctx;

static OSStatus putBytes(Ctx *c, const void *v, UInt32 n)
{
    if (c->sizeOnly) { *c->outSize = n; return 0; }
    if (c->in < n) return kAudioHardwareBadPropertySizeError;
    memcpy(c->out, v, n); *c->outSize = n; return 0;
}
static OSStatus putU32(Ctx *c, UInt32 v) { return putBytes(c, &v, 4); }
static OSStatus putF64(Ctx *c, Float64 v) { return putBytes(c, &v, 8); }
static OSStatus putCF(Ctx *c, CFStringRef s) { if (!c->sizeOnly && c->in < sizeof s) return kAudioHardwareBadPropertySizeError; if (!c->sizeOnly) CFRetain(s); return putBytes(c, &s, sizeof s); }
static OSStatus putIDs(Ctx *c, const UInt32 *ids, UInt32 n)     // object list that may be truncated to the caller's buffer
{
    if (c->sizeOnly) { *c->outSize = n * 4; return 0; }
    UInt32 fit = c->in / 4; if (fit > n) fit = n;
    memcpy(c->out, ids, fit * 4); *c->outSize = fit * 4; return 0;
}
static AudioStreamBasicDescription fmt(void)
{
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate; f.mFormatID = kAudioFormatLinearPCM; f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 4 * kChannels; f.mFramesPerPacket = 1; f.mChannelsPerFrame = kChannels; f.mBitsPerChannel = 32;
    return f;
}

static OSStatus getProp(AudioObjectID obj, const AudioObjectPropertyAddress *a, Ctx *c)
{
    const AudioObjectPropertyScope scope = a->mScope;
    switch (obj) {
    case kObjPlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return putU32(c, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return putU32(c, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: return putU32(c, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: return putCF(c, CFSTR("OhMyDSP"));
        case kAudioObjectPropertyOwnedObjects: case kAudioPlugInPropertyDeviceList: { UInt32 id = kObjDevice; return putIDs(c, &id, atomic_load(&gClockPeriod) ? 1 : 0); }
        case kAudioPlugInPropertyTranslateUIDToDevice: { UInt32 id = atomic_load(&gClockPeriod) ? kObjDevice : kAudioObjectUnknown; return putU32(c, id); }   // only one device; the UID qualifier is not checked
        case kAudioPlugInPropertyResourceBundle: return putCF(c, CFSTR(""));
        }
        return UNKNOWN;
    case kObjDevice:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return putU32(c, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return putU32(c, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: return putU32(c, kObjPlugIn);
        case kAudioObjectPropertyName: return putCF(c, CFSTR("Internal Microphone (DMIC)"));
        case kAudioObjectPropertyManufacturer: return putCF(c, CFSTR("OhMyDSP"));
        case kAudioObjectPropertyOwnedObjects: { UInt32 ids[4] = { kObjStream, kObjVolume, kObjMute, kObjSource }; return scope == kAudioObjectPropertyScopeOutput ? putIDs(c, ids, 0) : putIDs(c, ids, 4); }
        case kAudioDevicePropertyDeviceUID: return putCF(c, CFSTR("com.ohmydsp.InternalMicrophone"));
        case kAudioDevicePropertyModelUID: return putCF(c, CFSTR("com.ohmydsp.InternalMicrophone.model"));
        case kAudioDevicePropertyTransportType: return putU32(c, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: { UInt32 id = kObjDevice; return putIDs(c, &id, 1); }
        case kAudioDevicePropertyClockDomain: return putU32(c, 0);
        case kAudioDevicePropertyDeviceIsAlive: return putU32(c, 1);
        case kAudioDevicePropertyDeviceIsRunning: return putU32(c, gIO > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: return putU32(c, scope == kAudioObjectPropertyScopeOutput ? 0 : 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return putU32(c, scope == kAudioObjectPropertyScopeOutput ? 0 : 1);
        case kAudioDevicePropertyLatency: return putU32(c, 0);
        case kAudioDevicePropertyStreams: { UInt32 id = kObjStream; return scope == kAudioObjectPropertyScopeOutput ? putIDs(c, &id, 0) : putIDs(c, &id, 1); }
        case kAudioObjectPropertyControlList: { UInt32 ids[3] = { kObjVolume, kObjMute, kObjSource }; return scope == kAudioObjectPropertyScopeOutput ? putIDs(c, ids, 0) : putIDs(c, ids, 3); }
        case kAudioDevicePropertySafetyOffset: return putU32(c, scope == kAudioObjectPropertyScopeOutput ? 0 : kSafetyOffset);
        case kAudioDevicePropertyNominalSampleRate: return putF64(c, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: { AudioValueRange r = { kRate, kRate }; return putBytes(c, &r, sizeof r); }
        case kAudioDevicePropertyIsHidden: return putU32(c, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: return putU32(c, atomic_load(&gClockPeriod));
        case kAudioDevicePropertyClockAlgorithm:
#ifdef OHMYDSP_SMOOTH_CLOCK_EXPERIMENT
            return putU32(c, kAudioDeviceClockAlgorithmRaw);
#else
            return putU32(c, kAudioDeviceClockAlgorithmSimpleIIR); // baseline host filtering of measured lap anchors
#endif
        case kAudioDevicePropertyPreferredChannelsForStereo: { UInt32 ch[2] = { 1, 2 }; return putBytes(c, ch, sizeof ch); }
        }
        return UNKNOWN;
    case kObjStream:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return putU32(c, kAudioObjectClassID);
        case kAudioObjectPropertyClass: return putU32(c, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: return putU32(c, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: return putIDs(c, NULL, 0);
        case kAudioStreamPropertyIsActive: return putU32(c, 1);
        case kAudioStreamPropertyDirection: return putU32(c, 1);              // 1 = input
        case kAudioStreamPropertyTerminalType: return putU32(c, kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: return putU32(c, 1);
        case kAudioStreamPropertyLatency: return putU32(c, 0);
        case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: { AudioStreamBasicDescription f = fmt(); return putBytes(c, &f, sizeof f); }
        case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = { fmt(), { kRate, kRate } }; return putBytes(c, &r, sizeof r); }
        }
        return UNKNOWN;
    case kObjVolume: case kObjMute: case kObjSource:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: return putU32(c, obj == kObjVolume ? kAudioLevelControlClassID : obj == kObjMute ? kAudioBooleanControlClassID : kAudioSelectorControlClassID);
        case kAudioObjectPropertyClass: return putU32(c, obj == kObjVolume ? kAudioVolumeControlClassID : obj == kObjMute ? kAudioMuteControlClassID : kAudioDataSourceControlClassID);
        case kAudioObjectPropertyOwner: return putU32(c, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: return putIDs(c, NULL, 0);
        case kAudioControlPropertyScope: return putU32(c, kAudioObjectPropertyScopeInput);
        case kAudioControlPropertyElement: return putU32(c, kAudioObjectPropertyElementMain);
        }
        if (obj == kObjVolume) switch (a->mSelector) {
        case kAudioLevelControlPropertyScalarValue: { Float32 v = (gGainDb - kDbMin) / (kDbMax - kDbMin); return putBytes(c, &v, 4); }
        case kAudioLevelControlPropertyDecibelValue: return putBytes(c, &gGainDb, 4);
        case kAudioLevelControlPropertyDecibelRange: { AudioValueRange r = { kDbMin, kDbMax }; return putBytes(c, &r, sizeof r); }
        case kAudioLevelControlPropertyConvertScalarToDecibels: {       // value in, converted value out (same buffer)
            if (c->sizeOnly) return putU32(c, 4); if (c->in < 4) return kAudioHardwareBadPropertySizeError;
            Float32 v = *(Float32 *)c->out; if (v < 0) v = 0; if (v > 1) v = 1; v = kDbMin + v * (kDbMax - kDbMin); return putBytes(c, &v, 4); }
        case kAudioLevelControlPropertyConvertDecibelsToScalar: {
            if (c->sizeOnly) return putU32(c, 4); if (c->in < 4) return kAudioHardwareBadPropertySizeError;
            Float32 v = *(Float32 *)c->out; if (v < kDbMin) v = kDbMin; if (v > kDbMax) v = kDbMax; v = (v - kDbMin) / (kDbMax - kDbMin); return putBytes(c, &v, 4); }
        }
        if (obj == kObjMute && a->mSelector == kAudioBooleanControlPropertyValue) return putU32(c, gMute);
        if (obj == kObjSource) switch (a->mSelector) {
        case kAudioSelectorControlPropertyCurrentItem: case kAudioSelectorControlPropertyAvailableItems: return putU32(c, kSourceID);
        case kAudioSelectorControlPropertyItemName: if (c->qSize >= 4 && *(const UInt32 *)c->qData == kSourceID) return putCF(c, CFSTR("Internal Microphone (DMIC)")); return kAudioHardwareIllegalOperationError;
        }
        return UNKNOWN;
    }
    return kAudioHardwareBadObjectError;
}

static Boolean isSettable(AudioObjectID o, const AudioObjectPropertyAddress *a)
{
    return (o == kObjVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           (o == kObjMute && a->mSelector == kAudioBooleanControlPropertyValue) || (o == kObjSource && a->mSelector == kAudioSelectorControlPropertyCurrentItem);
}

static void saveGain(void)
{
    if (!gHost) return;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat32Type, &gGainDb);
    if (n) { gHost->WriteToStorage(gHost, CFSTR("gainDb"), n); CFRelease(n); }
}

// ---------------------------------------------------------------- driver interface
static HRESULT QueryInterface(void *d, REFIID iid, LPVOID *out);
static ULONG AddRef(void *d) { (void)d; return 1; }
static ULONG Release(void *d) { (void)d; return 1; }
static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    (void)d; gHost = host;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ discoverClock(); });
    mach_timebase_info_data_t tb; mach_timebase_info(&tb); gNsPerTick=(double)tb.numer/tb.denom;
    setGainDb(kDbDefault);
    CFPropertyListRef v = NULL;
    if (host && host->CopyFromStorage(host, CFSTR("gainDb"), &v) == 0 && v) {
        Float32 g; if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberFloat32Type, &g)) setGainDb(g);
        CFRelease(v);
    }
    LOG("initialized, input gain %.1f dB", gGainDb);
    return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *o) { (void)d; (void)desc; (void)ci; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) { (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci) { (void)d; (void)o; (void)ci; return 0; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *ci) { (void)d; (void)o; (void)ci; return 0; }
static OSStatus PerformConfigChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) { (void)d; (void)o; (void)a; (void)i; return 0; }
static OSStatus AbortConfigChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) { (void)d; (void)o; (void)a; (void)i; return 0; }

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress *a)
{ (void)d; (void)p; UInt32 sz = 0; Ctx c = { 0, &sz, NULL, true, 0, NULL }; return getProp(o, a, &c) == 0; }
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress *a, Boolean *s)
{ (void)d; (void)p; UInt32 sz = 0; Ctx c = { 0, &sz, NULL, true, 0, NULL }; OSStatus r = getProp(o, a, &c); *s = isSettable(o, a); return r; }
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 *sz)
{ (void)d; (void)p; Ctx c = { 0, sz, NULL, true, q, qd }; return getProp(o, a, &c); }
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 in, UInt32 *out, void *data)
{ (void)d; (void)p; Ctx c = { in, out, data, false, q, qd }; return getProp(o, a, &c); }
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t p, const AudioObjectPropertyAddress *a, UInt32 q, const void *qd, UInt32 sz, const void *data)
{
    (void)d; (void)p; (void)q; (void)qd;
    if (!isSettable(o, a)) return kAudioHardwareUnsupportedOperationError;
    if (sz != 4) return kAudioHardwareBadPropertySizeError;
    AudioObjectPropertyAddress ch[2]; UInt32 n = 0;
    if (o == kObjVolume) {
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyScalarValue) { if (v < 0) v = 0; if (v > 1) v = 1; setGainDb(kDbMin + v * (kDbMax - kDbMin)); } else setGainDb(v);
        saveGain();
        ch[n++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        ch[n++] = (AudioObjectPropertyAddress){ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        LOG("input gain set to %.1f dB", gGainDb);
    } else if (o == kObjMute) {
        gMute = *(const UInt32 *)data ? 1 : 0;
        ch[n++] = (AudioObjectPropertyAddress){ kAudioBooleanControlPropertyValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        LOG("input mute %u", (unsigned)gMute);
    } else if (*(const UInt32 *)data != kSourceID) return kAudioHardwareIllegalOperationError;
    if (n && gHost) gHost->PropertiesChanged(gHost, o, n, ch);
    return 0;
}

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    OSStatus r = 0;
    pthread_mutex_lock(&gLock);
    if(gIO==0) {
        atomic_store(&gDiagEnabled,false);
        bool reset=false;
        for(unsigned attempt=0;attempt<100;attempt++) {
            if(!atomic_flag_test_and_set_explicit(&gDiagLock,memory_order_acquire)) {
                memset(&gDiag,0,sizeof(gDiag));atomic_store(&gDiagSkipped,0);diagLeave();reset=true;break;
            }
            usleep(100);
        }
        if(!reset){pthread_mutex_unlock(&gLock);return kAudioHardwareNotRunningError;}
        gDiagID=gKernelSession=0;
    }
    if (gIO == 0 && !linkStart()) r = kAudioHardwareNotRunningError;
    else { gIO++; atomic_store(&gDiagEnabled,true); }
    pthread_mutex_unlock(&gLock);
    LOG("StartIO -> %d (clients %u)", (int)r, (unsigned)gIO);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    (void)d; (void)client;
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIO > 0 && --gIO == 0) { atomic_store(&gDiagEnabled,false); linkStop(); diagDump(); }
    pthread_mutex_unlock(&gLock);
    LOG("StopIO (clients %u)", (unsigned)gIO);
    return 0;
}

// Export the daemon's clock pair with an immutable declared period. Ring geometry never changes the advertised period.
// Only a hardware epoch change changes the seed; raw anchors remain available in diagnostic logs.
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *st, UInt64 *ht, UInt64 *seed)
{
    (void)d; (void)dev; (void)client;
    const volatile struct OhMyDSPShm *h = gShm;
    if (!h) return kAudioHardwareNotRunningError;
    struct OMDPosition p;
    if(!omdSnapshot(h,&p)) {
        if(diagEnter()){gDiag.clockSnapshotFailures++;diagLeave();}
        return kAudioHardwareNotRunningError;
    }
    if(diagEnter()){omdClock(&gDiag,&p,gRingFrames,gNsPerTick);diagLeave();}
    if(!p.clockValid || (p.state!=OHMYDSP_STATE_RUNNING && p.state!=OHMYDSP_STATE_SILENT))return kAudioHardwareNotRunningError;
    *st = (Float64)p.clockSample; *ht = p.clockHost; *seed = (UInt64)p.epoch + 1;
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace)
{ (void)d; (void)dev; (void)client; *will = op == kAudioServerPlugInIOOperationReadInput; *inPlace = true; return 0; }
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci)
{ (void)d; (void)dev; (void)client; (void)op; (void)n; (void)ci; return 0; }
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 n, const AudioServerPlugInIOCycleInfo *ci)
{ (void)d; (void)dev; (void)client; (void)op; (void)n; (void)ci; return 0; }

// The two microphones are one array: join them into a single signal and give it to both output channels. While a channel is digitally silent
// for the whole chunk (mic A has been exactly zero so far) the other one is used alone; as soon as both carry signal they are averaged.
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *ci, void *buf, void *unused)
{
    (void)d; (void)dev; (void)stream; (void)client; (void)unused;
    if (op != kAudioServerPlugInIOOperationReadInput) return 0;
    float *dst = buf; const volatile struct OhMyDSPShm *h = gShm;
    struct OMDPosition p={0};
    bool snapshot=h && gRing && omdSnapshot(h,&p);
    double sample=ci->mInputTime.mSampleTime;
    if(!isfinite(sample) || sample < -1e15 || sample > 1e15) snapshot=false;
    const int64_t first=snapshot?(int64_t)sample:0, written=(int64_t)p.written, rf=gRingFrames;
    if(diagEnter()){omdRead(&gDiag,&p,first,frames,rf,snapshot);diagLeave();}
    if(!snapshot || p.state!=1){memset(dst,0,frames*8);return 0;}
    const int32_t *ring = (const int32_t *)gRing;
    bool aAny = false, bAny = false;
    for (UInt32 i = 0; i < frames; i++) {                                                   // pass 1: which channel carries signal in this chunk
        int64_t s = first + i; if (s < 0 || s >= written || s < written - rf + 480) continue;
        int64_t k = s % rf; aAny |= ring[2 * k] != 0; bAny |= ring[2 * k + 1] != 0;
    }
    const float k1 = gMute ? 0.0f : gGainLin * (1.0f / 2147483648.0f);
    const float ka = (aAny && bAny) ? 0.5f * k1 : aAny ? k1 : 0.0f, kb = (aAny && bAny) ? 0.5f * k1 : bAny ? k1 : 0.0f;
    for (UInt32 i = 0; i < frames; i++) {
        int64_t s = first + i; float m = 0.0f;
        if (s >= 0 && s < written && s >= written - rf + 480) { int64_t k = s % rf; m = (float)ring[2 * k] * ka + (float)ring[2 * k + 1] * kb; }
        if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;
        dst[2 * i] = m; dst[2 * i + 1] = m;
    }
    return 0;
}

static AudioServerPlugInDriverInterface gIface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformConfigChange, AbortConfigChange, HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation };
static AudioServerPlugInDriverInterface *gIfacePtr = &gIface;
static AudioServerPlugInDriverRef gDriver = &gIfacePtr;

static HRESULT QueryInterface(void *d, REFIID iid, LPVOID *out)
{
    (void)d;
    CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(u, IUnknownUUID) || CFEqual(u, kAudioServerPlugInDriverInterfaceUUID)) { *out = gDriver; r = S_OK; }
    CFRelease(u);
    return r;
}

void *OhMyDSPDriverFactory(CFAllocatorRef alloc, CFUUIDRef type)
{
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? gDriver : NULL;
}
