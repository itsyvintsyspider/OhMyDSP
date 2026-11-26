// Exercise the actual HAL callbacks without a CoreAudio device, daemon, or microphone.
#include <xpc/xpc.h>
#include <sys/mman.h>
static xpc_connection_t fixtureConnection(const char *, dispatch_queue_t, uint64_t);
static xpc_object_t fixtureReply(xpc_connection_t, xpc_object_t);
#define xpc_connection_create_mach_service fixtureConnection
#define xpc_connection_send_message_with_reply_sync fixtureReply
#include "../ohmydsp/userspace/driver/OhMyDSPDriver.c"
#undef xpc_connection_create_mach_service
#undef xpc_connection_send_message_with_reply_sync
#include <assert.h>
#include <stdlib.h>

static UInt32 advertised, headerPeriod;
static unsigned queries, starts, stops;
static xpc_connection_t fixtureConnection(const char *name, dispatch_queue_t q, uint64_t flags)
{
    (void)name; (void)flags;
    return xpc_connection_create(NULL, q);
}
static xpc_object_t fixtureReply(xpc_connection_t c, xpc_object_t msg)
{
    (void)c;
    const char *cmd=xpc_dictionary_get_string(msg,"cmd");
    xpc_object_t reply=xpc_dictionary_create(NULL,NULL,0);
    xpc_dictionary_set_int64(reply,"status",0);
    if (!strcmp(cmd,"clock_info")) {
        queries++;
        xpc_dictionary_set_uint64(reply,"period",advertised);
    } else if (!strcmp(cmd,"start")) {
        starts++;
        size_t size=OHMYDSP_SHM_HEADER+advertised*8;
        struct OhMyDSPShm *h=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);
        assert(h!=MAP_FAILED);
        *h=(struct OhMyDSPShm){.magic=OHMYDSP_SHM_MAGIC,.version=OHMYDSP_SHM_VERSION,
            .channels=2,.frameBytes=8,.rate=48000,.ringBytes=advertised*8,.clockPeriod=headerPeriod};
        xpc_object_t region=xpc_shmem_create(h,size);
        xpc_dictionary_set_value(reply,"shm",region);
        xpc_release(region);
        munmap(h,size);
    } else {assert(!strcmp(cmd,"stop"));stops++;}
    return reply;
}
static OSStatus changed(AudioServerPlugInHostRef host,AudioObjectID obj,UInt32 count,
                        const AudioObjectPropertyAddress *addresses)
{
    (void)host;
    assert(obj==kObjPlugIn && count==2 && addresses[0].mSelector==kAudioPlugInPropertyDeviceList);
    return 0;
}
static void testNegotiation(UInt32 period)
{
    static const AudioServerPlugInHostInterface host={.PropertiesChanged=changed};
    gHost=&host;
    advertised=headerPeriod=period;
    starts=stops=queries=0;
    atomic_store(&gClockPeriod,0);
    discoverClock();
    assert(queries==1 && starts==0 && stops==0);
    AudioObjectPropertyAddress property={kAudioDevicePropertyZeroTimeStampPeriod,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 value=0,size=0;
    Ctx ctx={sizeof(value),&size,&value,false,0,NULL};
    assert(getProp(kObjDevice,&property,&ctx)==0 && value==period);
    assert(StartIO(NULL,kObjDevice,1)==0 && starts==1 && gShm->clockPeriod==period);
    assert(getProp(kObjDevice,&property,&ctx)==0 && value==period);
    assert(StopIO(NULL,kObjDevice,1)==0 && stops==1);
    headerPeriod=period+1;
    assert(StartIO(NULL,kObjDevice,1)!=0 && !gShm && !gIO);
}
static void resetDiag(void) {
    memset(&gDiag,0,sizeof(gDiag));atomic_store(&gDiagEnabled,true);atomic_store(&gDiagSkipped,0);atomic_flag_clear(&gDiagLock);
}
int main(void) {
    testNegotiation(16384);
    testNegotiation(32768);
    testNegotiation(131072);
    struct OhMyDSPShm h={0}; h.state=1;h.epoch=3;h.written=10000;h.lapHostTime=1000000000;h.clockValid=1;h.clockSample=512;h.clockHost=1010666667;
    int32_t *ring=malloc(1048576);assert(ring);
    for(unsigned i=0;i<1048576/4;i++)ring[i]=100000000;
    gShm=&h;gRing=(uint8_t *)ring;gRingFrames=131072;gNsPerTick=1;
    float output[200];AudioServerPlugInIOCycleInfo ci={0};
    resetDiag();ci.mInputTime.mSampleTime=9950;
    assert(DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL)==0);
    assert(gDiag.future==50 && gDiag.steadyMissing==0 && gDiag.events[0].p.written==10000);
    for(unsigned i=0;i<100;i++)assert((output[i*2]==0)==(i>=50));
    resetDiag();h.written=141072;ci.mInputTime.mSampleTime=10440;
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.stale==40 && gDiag.future==0);
    for(unsigned i=0;i<100;i++)assert((output[i*2]==0)==(i<40));
    resetDiag();h.written=10000;ci.mInputTime.mSampleTime=-20;
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.negative==20 && gDiag.future==0);
    resetDiag();h.written=30000;ci.mInputTime.mSampleTime=29950;
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.future==50 && gDiag.steadyMissing==50);
    resetDiag();h.state=3;
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.stateFrames==100 && output[0]==0);
    resetDiag();h.state=1;h.seq=1;
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.snapshotFrames==100);
    Float64 sample;UInt64 host,seed;
    assert(GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed)!=0 && gDiag.clockSnapshotFailures==1);
    // Pause the writer with an incomplete primary position for an entire callback.
    // The matching spare must supply both PCM bounds and the clock pair.
    h.nextPosition=(struct OMDPosition){.epoch=9,.state=1,.written=45000,.clockValid=1,.clockSample=1024,.clockHost=1234567890};
    h.nextSeq=h.seq;
    AudioServerPlugInIOCycleInfo contended=ci;
    contended.mInputTime.mSampleTime=40000;
    resetDiag();
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&contended,output,NULL);
    assert(gDiag.snapshotFrames==0 && gDiag.future==0 && gDiag.reads==1);
    for (unsigned i=0;i<200;i++) assert(output[i]>0);
    assert(GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed)==0 && sample==1024 && host==1234567890 && seed==10);
    h.seq=2;resetDiag();
    assert(GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed)==0 && sample==512 && host==1010666667 && seed==4);
    h.laps=1;h.lapHostTime+=2730666667ULL+200000;
    GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed);
    assert(sample==512 && gDiag.anchors==2 && fabs(gDiag.maxAbsResidualNs-200000.333333)<1);
    GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed);assert(gDiag.anchors==2);
    h.epoch++;h.laps=0;h.lapHostTime=1;GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed);
    assert(gDiag.clockRegressions==0 && seed==5);
    resetDiag();atomic_flag_test_and_set(&gDiagLock);
    DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(atomic_load(&gDiagSkipped)==1);atomic_flag_clear(&gDiagLock);
    resetDiag();for(unsigned i=0;i<70;i++)DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
    assert(gDiag.future==3500 && gDiag.count==64 && gDiag.dropped==6);
    AudioObjectPropertyAddress period={kAudioDevicePropertyZeroTimeStampPeriod,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 value=0,size=0;Ctx ctx={sizeof(value),&size,&value,false,0,NULL};
    for (unsigned i=0; i<3; i++) {
        const UInt32 frames = (UInt32[]){16384,32768,131072}[i];
        atomic_store(&gClockPeriod,frames);
        gRingFrames=0;
        assert(getProp(kObjDevice,&period,&ctx)==0 && value==frames);
        gRingFrames=frames;
        assert(getProp(kObjDevice,&period,&ctx)==0 && value==frames);
        h.state=OHMYDSP_STATE_SILENT; h.epoch++; h.clockValid=1;
        for(unsigned lap=0;lap<=2;lap++) {
            h.clockSample=(uint64_t)lap*frames; h.clockHost+=frames*1000000000ULL/48000;
            assert(GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed)==0);
            assert(sample==h.clockSample && host==h.clockHost && seed==h.epoch+1);
            memset(output,0xff,sizeof(output));
            DoIOOperation(NULL,2,3,1,kAudioServerPlugInIOOperationReadInput,100,&ci,output,NULL);
            for(unsigned j=0;j<200;j++)assert(output[j]==0);
        }
    }
    period.mSelector=kAudioDevicePropertyClockAlgorithm;assert(getProp(kObjDevice,&period,&ctx)==0 && value==kAudioDeviceClockAlgorithmSimpleIIR);
    h.clockValid=0;assert(GetZeroTimeStamp(NULL,2,1,&sample,&host,&seed)!=0);
    gShm=NULL;gRing=NULL;free(ring);
    puts("HAL actual-callback diagnostics: unavailable ranges, rendered zeros, steady counts, bounded reads/events, clock residuals, epoch reset and contention PASS");
}
