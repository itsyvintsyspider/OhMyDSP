// Records N seconds from the CoreAudio input device whose manufacturer is "OhMyDSP" (or the name given) and prints level statistics.
// Optional 3rd argument: file to save the capture as raw S32_LE 2 ch 48 kHz (python3 -I tools/raw2wav.py <file> <out.wav>).
// Build: clang -o /tmp/recordtest tools/recordtest.c -framework CoreAudio -framework CoreFoundation     Run: /tmp/recordtest [seconds] [name-substring]
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

static double sumsq[8], peak[8]; static unsigned long frames, calls, nonzero; static unsigned chans = 2;
static int *rec; static unsigned long recCap, recFrames;   // optional capture as S32 (for tools/raw2wav.py)

static OSStatus io(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inT, AudioBufferList *out, const AudioTimeStamp *outT, void *ctx)
{
    calls++;
    for (UInt32 b = 0; b < in->mNumberBuffers; b++) {
        const float *f = (const float *)in->mBuffers[b].mData; UInt32 n = in->mBuffers[b].mDataByteSize / 4, ch = in->mBuffers[b].mNumberChannels;
        for (UInt32 i = 0; i < n; i++) { double v = f[i]; unsigned c = i % ch; if (c < 8) { sumsq[c] += v * v; if (fabs(v) > peak[c]) peak[c] = fabs(v); } if (v != 0) nonzero++; }
        if (rec && ch == 2) for (UInt32 fr = 0; fr < n / 2 && recFrames < recCap; fr++, recFrames++)
            for (int c = 0; c < 2; c++) { double v = f[fr * 2 + c] * 2147483647.0; if (v > 2147483647.0) v = 2147483647.0; if (v < -2147483648.0) v = -2147483648.0; rec[recFrames * 2 + c] = (int)v; }
        frames += n / ch; chans = ch;
    }
    return noErr;
}

static CFStringRef str(AudioObjectID o, AudioObjectPropertySelector s) {
    AudioObjectPropertyAddress a = { s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }; CFStringRef r = NULL; UInt32 sz = sizeof r;
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &r) == noErr ? r : NULL; }

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 2; const char *want = argc > 2 ? argv[2] : "OhMyDSP"; const char *outf = argc > 3 ? argv[3] : NULL;
    if (outf) { recCap = (unsigned long)(secs + 1) * 48000; rec = calloc(recCap * 2, sizeof(int)); }
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID *ids = malloc(sz); AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    AudioObjectID dev = 0;
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
        char name[256] = "", man[256] = ""; CFStringRef n = str(ids[i], kAudioObjectPropertyName), m = str(ids[i], kAudioObjectPropertyManufacturer);
        if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8); if (m) CFStringGetCString(m, man, sizeof man, kCFStringEncodingUTF8);
        AudioObjectPropertyAddress ia = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain }; UInt32 isz = 0; AudioObjectGetPropertyDataSize(ids[i], &ia, 0, NULL, &isz);
        printf("device %u: '%s' by '%s' input streams %u\n", ids[i], name, man, (unsigned)(isz / sizeof(AudioObjectID)));
        if (isz && (strstr(man, want) || strstr(name, want))) dev = ids[i];
    }
    if (!dev) { printf("no input device matching '%s'\n", want); return 1; }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }; Float64 rate = 0; sz = sizeof rate;
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate); printf("using device %u, nominal rate %.0f\n", dev, rate);
    AudioDeviceIOProcID pid; if (AudioDeviceCreateIOProcID(dev, io, NULL, &pid) != noErr) { printf("create IOProc failed\n"); return 1; }
    OSStatus st = AudioDeviceStart(dev, pid); if (st != noErr) { printf("start failed %d\n", (int)st); return 1; }
    sleep(secs); AudioDeviceStop(dev, pid);
    if (outf && rec) { FILE *fo = fopen(outf, "wb"); if (fo) { fwrite(rec, 8, recFrames, fo); fclose(fo); printf("saved %lu frames to %s\n", recFrames, outf); } } AudioDeviceDestroyIOProcID(dev, pid);
    printf("IOProc calls %lu, frames %lu (expected ~%.0f), channels %u, non-zero samples %lu\n", calls, frames, rate * secs, chans, nonzero);
    for (unsigned c = 0; c < chans && c < 8; c++) printf("ch%u: peak %.4f (%.1f dBFS)  rms %.4f (%.1f dBFS)\n", c, peak[c], 20 * log10(peak[c] + 1e-12), sqrt(sumsq[c] / (frames ? frames : 1)), 20 * log10(sqrt(sumsq[c] / (frames ? frames : 1)) + 1e-12));
    return 0;
}
