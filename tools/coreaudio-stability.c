// Real CoreAudio input qualification; no direct user client, default-device or format changes.
// Build with tools/build-coreaudio-stability.sh. Run --list or --self-test without recording.
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define UID "com.ohmydsp.InternalMicrophone"
#define RATE 48000.0
#define WARMUP 24000u
#define MAX_SAMPLE_FRAMES 480000u
#define MAX_EVENTS 64
// Bounded callback evidence; written without allocation and exported only after IOProc teardown.
typedef struct { unsigned kind; uint64_t frame; double sample, delta; } Event;
static volatile sig_atomic_t interrupted;
static void onSignal(int sig) { (void)sig; interrupted = 1; }
typedef struct {
    uint64_t calls, frames, malformed, invalidTime, discontinuities, nonfinite;
    uint64_t steadyCalls, steadyFrames, zeroFrames, longestZero, zeroRun, clipped[2];
    double sumsq[2], peak[2], nextSample, firstSample, lastSample;
    uint64_t firstHost, lastHost, previousCallback, maxGap;
    bool haveSample, haveHost;
    float *sample; uint32_t sampleFrames;
    Event events[MAX_EVENTS]; unsigned eventCount, eventsDropped;
    atomic_uint_fast64_t progress;
} Stats;
static void event(Stats *s, unsigned kind, uint64_t frame, double sample, double delta)
{
    if (s->eventCount < MAX_EVENTS) s->events[s->eventCount++] = (Event){kind,frame,sample,delta};
    else s->eventsDropped++;
}
static mach_timebase_info_data_t timebase;
static double seconds(uint64_t t) { return (double)t * timebase.numer / timebase.denom / 1e9; }
// Only the audio callback writes statistics; main reads them after stopping/destroying the IOProc.
static void consume(Stats *s, const AudioBufferList *in, const AudioTimeStamp *t, uint64_t now)
{
    s->calls++;
    if (!in || in->mNumberBuffers != 1 || in->mBuffers[0].mNumberChannels != 2 ||
        !in->mBuffers[0].mData || !in->mBuffers[0].mDataByteSize || in->mBuffers[0].mDataByteSize % 8) {
        s->malformed++; return;
    }
    uint32_t n = in->mBuffers[0].mDataByteSize / 8;
    bool steady = s->frames >= WARMUP;
    bool validSample = t && (t->mFlags & kAudioTimeStampSampleTimeValid) && isfinite(t->mSampleTime);
    bool validHost = t && (t->mFlags & kAudioTimeStampHostTimeValid);
    if (steady) {
        s->steadyCalls++; s->steadyFrames += n;
        if (!validSample || !validHost) s->invalidTime++;
        if (validSample && s->haveSample && fabs(t->mSampleTime - s->nextSample) > 0.5) {
            s->discontinuities++; event(s, 1, s->frames, t->mSampleTime, t->mSampleTime - s->nextSample);
        }
        if (s->previousCallback && now - s->previousCallback > s->maxGap) s->maxGap = now - s->previousCallback;
        if (validHost) {
            if (s->haveHost && t->mHostTime <= s->lastHost) {
                s->discontinuities++; event(s, 2, s->frames, validSample ? t->mSampleTime : 0,
                    -seconds(s->lastHost - t->mHostTime) * 1000);
            }
            if (!s->haveHost) s->firstHost = t->mHostTime;
            s->lastHost = t->mHostTime; s->haveHost = true;
        }
    }
    if (validSample) {
        if (!s->haveSample) s->firstSample = t->mSampleTime;
        s->lastSample = t->mSampleTime;
        s->nextSample = t->mSampleTime + n; s->haveSample = true;
    } else s->haveSample = false;
    const float *f = in->mBuffers[0].mData;
    for (uint32_t i = 0; i < n; i++) {
        bool zero = true;
        for (unsigned c = 0; c < 2; c++) {
            double v = f[2*i+c];
            if (!isfinite(v)) { s->nonfinite++; zero = false; continue; }
            if (steady) { s->sumsq[c] += v*v; if (fabs(v) > s->peak[c]) s->peak[c] = fabs(v); s->clipped[c] += fabs(v) >= 0.999999; }
            if (v != 0) zero = false;
        }
        if (steady) {
            if (zero) { s->zeroFrames++; s->zeroRun++; if (s->zeroRun > s->longestZero) s->longestZero = s->zeroRun; }
            else {
                if (s->zeroRun >= 48) event(s, 3, s->frames+i-s->zeroRun, validSample ? t->mSampleTime+i-s->zeroRun : 0, s->zeroRun / 48.0);
                s->zeroRun = 0;
            }
        }
        if (s->sample && s->sampleFrames < MAX_SAMPLE_FRAMES) {
            memcpy(s->sample + 2*s->sampleFrames, f + 2*i, 8); s->sampleFrames++;
        }
    }
    s->frames += n; s->previousCallback = now;
    atomic_store_explicit(&s->progress, s->frames, memory_order_relaxed);
}
static OSStatus callback(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                         const AudioTimeStamp *it, AudioBufferList *out, const AudioTimeStamp *ot, void *ctx)
{
    (void)dev; (void)now; (void)out; (void)ot;
    consume(ctx, in, it, mach_absolute_time()); return noErr;
}
static AudioObjectPropertyAddress address(AudioObjectPropertySelector s, AudioObjectPropertyScope scope)
{ return (AudioObjectPropertyAddress){s, scope, kAudioObjectPropertyElementMain}; }
static OSStatus readPeriod(AudioObjectID dev, UInt32 *period)
{
    AudioObjectPropertyAddress a=address(kAudioDevicePropertyZeroTimeStampPeriod,kAudioObjectPropertyScopeGlobal);
    UInt32 n=sizeof(*period);return AudioObjectGetPropertyData(dev,&a,0,NULL,&n,period);
}
static AudioObjectID findDevice(void)
{
    AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size)) return 0;
    AudioObjectID *ids = malloc(size); if (!ids) return 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids)) { free(ids); return 0; }
    AudioObjectID found = 0;
    for (UInt32 i = 0; i < size / sizeof(*ids); i++) {
        CFStringRef uid = NULL; UInt32 n = sizeof(uid);
        a = address(kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal);
        if (!AudioObjectGetPropertyData(ids[i], &a, 0, NULL, &n, &uid) && uid) {
            if (CFEqual(uid, CFSTR(UID))) found = ids[i]; CFRelease(uid);
        }
    }
    free(ids); return found;
}
static OSStatus checkFormat(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
    UInt32 n = 0; OSStatus r = AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &n);
    if (r) return r;
    if (n != sizeof(AudioStreamID)) return kAudioHardwareUnsupportedOperationError;
    AudioStreamID stream = 0; r = AudioObjectGetPropertyData(dev, &a, 0, NULL, &n, &stream); if (r) return r;
    AudioStreamBasicDescription f = {0}; n = sizeof(f);
    a = address(kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal);
    r = AudioObjectGetPropertyData(stream, &a, 0, NULL, &n, &f); if (r) return r;
    if (f.mSampleRate != RATE || f.mFormatID != kAudioFormatLinearPCM || f.mChannelsPerFrame != 2 ||
        f.mBytesPerFrame != 8 || f.mBitsPerChannel != 32 || !(f.mFormatFlags & kAudioFormatFlagIsFloat) ||
        (f.mFormatFlags & (kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagIsBigEndian))) return kAudioHardwareUnsupportedOperationError;
    return 0;
}
static void u16(FILE *f, uint16_t x) { unsigned char b[2] = {x, x>>8}; fwrite(b,1,2,f); }
static void u32(FILE *f, uint32_t x) { unsigned char b[4] = {x,x>>8,x>>16,x>>24}; fwrite(b,1,4,f); }
static bool saveWav(const char *path, const Stats *s)
{
    int fd = open(path, O_WRONLY|O_CREAT|O_EXCL, 0600); if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb"); if (!f) { close(fd); return false; }
    uint32_t bytes = s->sampleFrames * 8;
    fwrite("RIFF",1,4,f); u32(f,48+bytes); fwrite("WAVEfmt ",1,8,f); u32(f,16); u16(f,3); u16(f,2);
    u32(f,48000); u32(f,384000); u16(f,8); u16(f,32);
    fwrite("fact",1,4,f); u32(f,4); u32(f,s->sampleFrames);
    fwrite("data",1,4,f); u32(f,bytes); fwrite(s->sample,1,bytes,f);
    bool ok = !ferror(f); if (fclose(f)) ok = false; return ok;
}
static int selfTest(void)
{
    Stats s = {0}; float f[960]; for (unsigned i=0;i<960;i++) f[i]=0.25f;
    AudioBufferList in = {1, {{2, sizeof(f), f}}};
    AudioTimeStamp t = {0}; t.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
    uint64_t tick = (uint64_t)(1e7 * timebase.denom / timebase.numer);
    for (unsigned i=0;i<100;i++) { t.mSampleTime=i*480; t.mHostTime=(i+1)*tick; consume(&s,&in,&t,t.mHostTime); }
    if (s.frames != 48000 || s.steadyFrames != 24000 || s.discontinuities || s.nonfinite || s.zeroFrames || fabs(s.peak[0]-.25)>1e-9) return 1;
    t.mSampleTime += 960; t.mHostTime += tick; consume(&s,&in,&t,t.mHostTime);
    if (s.discontinuities != 1 || s.events[0].kind != 1 || s.events[0].delta != 480) return 2;
    memset(f,0,sizeof(f)); t.mSampleTime += 480; t.mHostTime += tick; consume(&s,&in,&t,t.mHostTime);
    if (s.zeroFrames != 480 || s.longestZero != 480) return 3;
    f[0] = NAN; f[1] = 1; t.mSampleTime += 480; t.mHostTime += tick; consume(&s,&in,&t,t.mHostTime);
    if (s.nonfinite != 1 || s.clipped[1] != 1) return 4;
    t.mFlags = 0; consume(&s,&in,&t,t.mHostTime); if (!s.invalidTime) return 5;
    in.mBuffers[0].mDataByteSize--; consume(&s,&in,&t,t.mHostTime); if (!s.malformed) return 6;
    puts("CoreAudio statistics self-test PASS (continuity, silence, clipping, NaN, timestamps, format)"); return 0;
}
int main(int argc, char **argv)
{
    mach_timebase_info(&timebase);
    if (argc==2 && !strcmp(argv[1],"--self-test")) return selfTest();
    if (argc==2 && !strcmp(argv[1],"--list")) {
        AudioObjectID d=findDevice(); OSStatus r=d?checkFormat(d):-1;
        printf("{\"device_uid\":\"%s\",\"device_id\":%u,\"format_status\":%d}\n",UID,(unsigned)d,(int)r); return r?1:0;
    }
    char *end = NULL; double duration = argc>1?strtod(argv[1],&end):0;
    if (argc<3 || argc>4 || !end || *end || !isfinite(duration) || duration<2 || duration>1800) {
        fprintf(stderr,"Usage: %s --list | --self-test | SECONDS(2..1800) NEW_REPORT.json [NEW_SAMPLE.wav]\n",argv[0]); return 2;
    }
    umask(077);
    int fd=open(argv[2],O_WRONLY|O_CREAT|O_EXCL,0600); if(fd<0){perror("report");return 2;}
    FILE *report=fdopen(fd,"w"); if(!report){close(fd);return 2;}
    signal(SIGINT,onSignal); signal(SIGTERM,onSignal);
    Stats *s=calloc(1,sizeof(*s)); if(!s){fclose(report);return 2;}
    if(argc==4) { s->sample=calloc(MAX_SAMPLE_FRAMES,8); if(!s->sample){free(s);fclose(report);return 2;} }
    AudioObjectID dev=findDevice(); AudioDeviceIOProcID proc=NULL;
    UInt32 periodBefore=0,periodAfter=0;OSStatus periodBeforeStatus=dev?readPeriod(dev,&periodBefore):-1,periodAfterStatus=-1;
    OSStatus setup=dev?checkFormat(dev):-1, start=-1, stop=-1, destroy=0;
    uint64_t a=mach_absolute_time(), started=0, ended=0; bool stalled=false;
    if(!setup) setup=AudioDeviceCreateIOProcID(dev,callback,s,&proc);
    if(!setup) {
        a=mach_absolute_time(); start=AudioDeviceStart(dev,proc); started=mach_absolute_time();
        if(!start) {
            uint64_t changed=started, last=0;
            while(!interrupted && seconds(mach_absolute_time()-started)<duration) {
                usleep(100000);
                uint64_t n=atomic_load_explicit(&s->progress,memory_order_relaxed), now=mach_absolute_time();
                if(n!=last){last=n;changed=now;}
                if(seconds(now-changed)>5){stalled=true;break;}
            }
            ended=mach_absolute_time(); stop=AudioDeviceStop(dev,proc);
        }
        destroy=AudioDeviceDestroyIOProcID(dev,proc);
    }
    if(dev)periodAfterStatus=readPeriod(dev,&periodAfter);
    // On a failed stop/destroy the callback might still be using Stats: exit the process without inspecting/freeing it.
    if (proc && (destroy || (!start && stop))) {
        fprintf(report,"{\"schema\":1,\"status\":\"FAIL\",\"stop_status\":%d,\"destroy_status\":%d,\"reason\":\"callback cleanup failed\"}\n",(int)stop,(int)destroy);
        fclose(report); _Exit(1);
    }
    if (s->zeroRun >= 48) event(s, 3, s->frames-s->zeroRun, s->haveSample ? s->nextSample-s->zeroRun : 0, s->zeroRun/48.0);
    double elapsed=ended?seconds(ended-started):0, ratio=elapsed?s->frames/(elapsed*RATE):0;
    bool pass=!setup&&!start&&!stop&&!destroy&&!stalled&&!interrupted&&s->steadyCalls>1&&
        !(periodBeforeStatus==0 && periodAfterStatus==0 && periodBefore!=periodAfter)&&!s->malformed&&!s->invalidTime&&!s->discontinuities&&!s->nonfinite&&ratio>=.95&&ratio<=1.05;
    bool wavOK=argc!=4||saveWav(argv[3],s); if(!wavOK) pass=false;
    fprintf(report,"{\"schema\":1,\"device_uid\":\"%s\",\"status\":\"%s\",\"setup_status\":%d,\"start_status\":%d,\"stop_status\":%d,\"destroy_status\":%d,\"interrupted\":%s,\"callback_stall\":%s,\"wav_ok\":%s,",UID,pass?"PASS":"FAIL",(int)setup,(int)start,(int)stop,(int)destroy,interrupted?"true":"false",stalled?"true":"false",wavOK?"true":"false");
    fprintf(report,"\"period_before_status\":%d,\"period_after_status\":%d,\"period_before\":%u,\"period_after\":%u,",(int)periodBeforeStatus,(int)periodAfterStatus,periodBefore,periodAfter);
    fprintf(report,"\"requested_seconds\":%.3f,\"elapsed_seconds\":%.6f,\"start_ms\":%.3f,\"frames\":%llu,\"frame_wall_ratio\":%.6f,\"callbacks\":%llu,\"warmup_frames\":%u,\"steady_frames\":%llu,\"malformed_buffers\":%llu,\"invalid_timestamps\":%llu,\"timestamp_discontinuities\":%llu,\"nonfinite_samples\":%llu,\"max_callback_gap_ms\":%.3f,\"zero_frames\":%llu,\"longest_zero_ms\":%.3f,\"channels\":[",duration,elapsed,started?seconds(started-a)*1000:0,(unsigned long long)s->frames,ratio,(unsigned long long)s->calls,WARMUP,(unsigned long long)s->steadyFrames,(unsigned long long)s->malformed,(unsigned long long)s->invalidTime,(unsigned long long)s->discontinuities,(unsigned long long)s->nonfinite,seconds(s->maxGap)*1000,(unsigned long long)s->zeroFrames,s->longestZero/48.0);
    for(unsigned c=0;c<2;c++) fprintf(report,"%s{\"rms_dbfs\":%.3f,\"peak_dbfs\":%.3f,\"clipped_samples\":%llu}",c?",":"",20*log10(sqrt(s->sumsq[c]/(s->steadyFrames?s->steadyFrames:1))+1e-15),20*log10(s->peak[c]+1e-15),(unsigned long long)s->clipped[c]);
    fprintf(report, "],\"events_dropped\":%u,\"events\":[", s->eventsDropped);
    for (unsigned i=0;i<s->eventCount;i++) {
        const Event *e=&s->events[i];
        fprintf(report, "%s{\"kind\":\"%s\",\"recorded_frame\":%llu,\"sample_time\":%.6f,\"delta\":%.6f}", i?",":"", e->kind==1?"sample_jump_frames":e->kind==2?"host_nonmonotonic_ms":"zero_run_ms", (unsigned long long)e->frame, e->sample, e->delta);
    }
    fprintf(report, "]}\n"); bool outputOK=!ferror(report); if(fclose(report)) outputOK=false;
    printf("%s: %.2fs, %llu frames, ratio %.5f, discontinuities %llu, max callback gap %.2fms\n",pass&&outputOK?"PASS":"FAIL",elapsed,(unsigned long long)s->frames,ratio,(unsigned long long)s->discontinuities,seconds(s->maxGap)*1000);
    free(s->sample); free(s); return pass&&outputOK?0:1;
}
