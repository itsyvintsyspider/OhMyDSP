// Phase 5: IOAudioDevice + input IOAudioEngine. See MicAudio.hpp for the two modes (synthetic tone / live DSP microphones).
// The ring layout, position polling and timestamp logic are the same in both modes (5.1 proved them with the tone).
#include "MicAudio.hpp"
#include "DSPDevice.hpp"
#include "Config.hpp"
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>

#define OLOG(fmt, ...) do { os_log(OS_LOG_DEFAULT, "OhMyDSP audio: " fmt, ##__VA_ARGS__); } while (0)

#define super IOAudioDevice
OSDefineMetaClassAndStructors(OhMyDSPAudioDevice, IOAudioDevice)
#undef super
#define super IOAudioEngine
OSDefineMetaClassAndStructors(OhMyDSPAudioEngine, IOAudioEngine)
#undef super

enum : UInt32 { RATE = 48000, FRAME_BYTES = 8, SYN_RING_BYTES = 1u << 17, TICK_MS = 5, SAFETY_FRAMES = 2400, LIVE_PCM_MSGS = 19, STALL_TICKS = 40, MAX_RECOVERIES = 3 };

// 1 kHz sine at 0.2 full scale, one 48-sample period
static const SInt32 kTone[48] = {
    0, 56060572, 111161933, 164361282, 214748364, 261461042, 303700049, 340742664, 371955078, 396803237, 414861983, 425822325,
    429496729, 425822325, 414861983, 396803237, 371955078, 340742664, 303700049, 261461042, 214748364, 164361282, 111161933, 56060572,
    0, -56060572, -111161933, -164361282, -214748364, -261461042, -303700049, -340742664, -371955078, -396803237, -414861983, -425822325,
    -429496729, -425822325, -414861983, -396803237, -371955078, -340742664, -303700049, -261461042, -214748364, -164361282, -111161933, -56060572 };

// 10^(n/20) for n = 0..40 dB (the kernel has no pow())
static const float kDb[41] = { 1.000000f, 1.122018f, 1.258925f, 1.412538f, 1.584893f, 1.778279f, 1.995262f, 2.238721f, 2.511886f, 2.818383f, 3.162278f, 3.548134f, 3.981072f, 4.466836f, 5.011872f, 5.623413f, 6.309573f, 7.079458f, 7.943282f, 8.912509f, 10.000000f, 11.220185f, 12.589254f, 14.125375f, 15.848932f, 17.782794f, 19.952623f, 22.387211f, 25.118864f, 28.183829f, 31.622777f, 35.481339f, 39.810717f, 44.668359f, 50.118723f, 56.234133f, 63.095734f, 70.794578f, 79.432823f, 89.125094f, 100.000000f };

static IOReturn gainChanged(OSObject *target, IOAudioControl *, SInt32, SInt32 newValue)
{
    OhMyDSPAudioEngine *e = OSDynamicCast(OhMyDSPAudioEngine, target);
    if (e) { e->gain = newValue; e->setGainFromSlider(newValue); OLOG("input volume -> %d", (int)newValue); }
    return kIOReturnSuccess;
}
static IOReturn sourceChanged(OSObject *, IOAudioControl *, SInt32, SInt32 newValue)
{
    OLOG("input source -> 0x%x", (unsigned)newValue); return kIOReturnSuccess;
}

static inline uint64_t nsToAbs(uint64_t ns) { uint64_t a = 0; nanoseconds_to_absolutetime(ns, &a); return a; }
static inline uint64_t absToNs(uint64_t a) { uint64_t n = 0; absolutetime_to_nanoseconds(a, &n); return n; }
static inline uint32_t msSince(uint64_t from) { uint64_t n; clock_get_uptime(&n); return (uint32_t)(absToNs(n - from) / 1000000ULL); }

// ---------------------------------------------------------------- device
bool OhMyDSPAudioDevice::initHardware(IOService *provider)
{
    if (!IOAudioDevice::initHardware(provider)) return false;
    setDeviceName("Internal Microphone");
    setDeviceShortName("Internal Mic");
    setManufacturerName("OhMyDSP");
    setDeviceTransportType(kIOAudioDeviceTransportTypeBuiltIn);
    OhMyDSPAudioEngine *eng = new OhMyDSPAudioEngine;
    if (!eng) return false;
    eng->dsp = dsp;
    if (!eng->init(NULL)) { eng->release(); return false; }
    IOReturn r = activateAudioEngine(eng);   // calls eng->initHardware(provider) via start()
    eng->release();
    OLOG("device ready (%s, activateAudioEngine -> 0x%x)", dsp ? "live DSP" : "synthetic", (unsigned)r);
    return r == kIOReturnSuccess;
}

// ---------------------------------------------------------------- engine
// Slider 0..100 -> software gain: 0 = mute, else (v - 57) / 2 dB (v = 75 -> +9 dB, v = 100 -> +21 dB; +17 dB clipped claps in the 5.3 test, +7 was clean, +2 dB added at the user's request). Speech from the DMICs is only about
// -40 dBFS RMS, apps expect far more. Step 5.4 moves the gain into the DSP (volume component) where it buys real headroom.
void OhMyDSPAudioEngine::applyGain()
{
    int db = gainDbOverride != 1000 ? (int)gainDbOverride : (gain - 57) / 2;
    if (db > 40) db = 40;
    if (db < -40) db = -40;
    float lin = db >= 0 ? kDb[db] : 1.0f / kDb[-db];
    gainLin = (gainDbOverride == 1000 && gain <= 0) ? 0.0f : lin;
}
void OhMyDSPAudioEngine::setGainFromSlider(SInt32 v) { gain = v; applyGain(); }

bool OhMyDSPAudioEngine::initHardware(IOService *provider)
{
    if (!IOAudioEngine::initHardware(provider)) return false;
    setDescription("Internal Microphone");
    tickMs = (UInt32)gCfg.tickMs; tsMode = (UInt32)gCfg.tsMode; rawMode = gCfg.raw;
    gain = 2 * gCfg.gainDb + 57; if (gain < 1) gain = 1; if (gain > 100) gain = 100;   // slider position of the requested dB (inverse of applyGain)
    IOAudioSampleRate rate = { RATE, 0 };
    setSampleRate(&rate);
    setProperty("IOAudioEngineDisableClockBoundsCheck", kOSBooleanTrue);   // timestamps come from a software timer, not an interrupt; same as AppleHDA
    setClockDomain(0x4F4D4453);                                            // fixed small value ('OMDS'), not a pointer
    setSampleOffset(SAFETY_FRAMES);
    setInputSampleOffset(SAFETY_FRAMES);

    if (dsp) {   // live: the engine's sample buffer IS the DMA ring (allocated once by the provider, stable address)
        ringBase = dsp->captureBase(); ringBytes = dsp->captureBytes();
        if (!ringBase || !ringBytes) { OLOG("live mode: DMA ring missing"); return false; }
    } else {
        ring = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, SYN_RING_BYTES, PAGE_SIZE);
        if (!ring) return false;
        ringBase = ring->getBytesNoCopy(); ringBytes = SYN_RING_BYTES;
        memset(ringBase, 0, ringBytes);
    }
    ringFrames = ringBytes / FRAME_BYTES;
    setNumSampleFramesPerBuffer(ringFrames);

    stream = new IOAudioStream;
    if (!stream || !stream->initWithAudioEngine(this, kIOAudioStreamDirectionInput, 1, "Internal Microphone")) return false;
    stream->setSampleBuffer(ringBase, ringBytes);
    stream->setTerminalType(INPUT_MICROPHONE);
    IOAudioStreamFormat fmt = {};
    fmt.fNumChannels = 2; fmt.fSampleFormat = kIOAudioStreamSampleFormatLinearPCM; fmt.fNumericRepresentation = kIOAudioStreamNumericRepresentationSignedInt;
    fmt.fBitDepth = 32; fmt.fBitWidth = 32; fmt.fAlignment = kIOAudioStreamAlignmentHighByte; fmt.fByteOrder = kIOAudioStreamByteOrderLittleEndian;
    fmt.fIsMixable = 1; fmt.fDriverTag = 0;
    stream->addAvailableFormat(&fmt, &rate, &rate);
    stream->setFormat(&fmt);
    addAudioStream(stream);
    stream->release();   // the engine owns it now

    // controls: input data source ("Internal Microphone") and input volume slider
    IOAudioSelectorControl *sel = IOAudioSelectorControl::createInputSelector(kIOAudioSelectorControlSelectionValueInternalMicrophone, kIOAudioControlChannelIDAll);
    if (sel) {
        sel->setValueChangeHandler(sourceChanged, this);
        sel->addAvailableSelection(kIOAudioSelectorControlSelectionValueInternalMicrophone, "Internal Microphone");
        addDefaultAudioControl(sel); sel->release();
    }
    IOAudioLevelControl *vol = IOAudioLevelControl::createVolumeControl(gain, 0, 100, -22 * 65536 - 32768, 0, kIOAudioControlChannelIDAll, "Input Volume", 0, kIOAudioControlUsageInput);
    if (vol) { vol->setValueChangeHandler(gainChanged, this); addDefaultAudioControl(vol); vol->release(); }
    applyGain();

    timer = IOTimerEventSource::timerEventSource(this, tickAction);
    if (!timer || getWorkLoop()->addEventSource(timer) != kIOReturnSuccess) return false;
    OLOG("engine ready (%s): %u Hz, 2 ch S32, %u frames ring (%u bytes)", dsp ? "live" : "synthetic", (unsigned)RATE, (unsigned)ringFrames, (unsigned)ringBytes);
    return true;
}

void OhMyDSPAudioEngine::stop(IOService *provider)
{
    if (timer) { timer->cancelTimeout(); getWorkLoop()->removeEventSource(timer); timer->release(); timer = nullptr; }
    running = false;
    if (dsp && active) { OLOG("engine torn down while running (provider terminated)"); bringDownLive(); }
    IOAudioEngine::stop(provider);
}

void OhMyDSPAudioEngine::free()
{
    if (ring) { ring->release(); ring = nullptr; }
    IOAudioEngine::free();
}

IOReturn OhMyDSPAudioEngine::performAudioEngineStart()
{
    OLOG("engine start (%s)", dsp ? "live" : "synthetic");
    produced = 0; wraps = 0; stallTicks = 0; errLogged = false; lastLpib = 0;
    if (!dsp) {
        running = true;
        clock_get_uptime(&t0);
        takeTimeStamp(false);                       // first timestamp, loop count not incremented
        timer->setTimeoutMS(tickMs);
        return kIOReturnSuccess;
    }
    silent = false;
    if (!bringUpLive()) return kIOReturnError;
    return kIOReturnSuccess;
}

// live: DSP power-up + firmware + pipeline + capture stream (all steps logged and bounded by DSPDevice). Used by engine start and by fault recovery.
// After a system wake AppleHDAController may still be bringing the controller back, so the idle check is retried a few times (bounded, 0.8 s).
bool OhMyDSPAudioEngine::bringUpLive()
{
    if (failStarts > 0) { failStarts--; OLOG("INJECT: live start refused (%d more to refuse)", (int)failStarts); return false; }
    uint64_t a, b, c, d;
    clock_get_uptime(&a);
    bool ok = false;
    for (int tryN = 0; tryN < 5 && !ok; tryN++) {
        if (tryN) { OLOG("controller not ready (try %d of 5), waiting 200 ms", tryN); IOSleep(200); }
        ok = dsp->checkIdle();
    }
    ok = ok && dsp->powerUp(false);
    clock_get_uptime(&b);
    ok = ok && dsp->sendPipeline(dsp->ipcMessages() < LIVE_PCM_MSGS ? 0 : LIVE_PCM_MSGS);
    clock_get_uptime(&c);
    ok = ok && dsp->startStream(4, (UInt32)gCfg.sd, 0);     // DMA RUN, then TRIG_START; ring starts as silence
    clock_get_uptime(&d);
    OLOG("engine start latency: power-up+firmware %u ms, pipeline %u ms, stream %u ms, total %u ms -> %s",
         (unsigned)(absToNs(b - a) / 1000000ULL), (unsigned)(absToNs(c - b) / 1000000ULL), (unsigned)(absToNs(d - c) / 1000000ULL), (unsigned)(absToNs(d - a) / 1000000ULL), ok ? "OK" : "FAILED");
    if (!ok) {
        dsp->freeStream(); dsp->teardown(false);
        return false;
    }
    active = true;
    UInt32 lp = 0, sts = 0; dsp->sample(&lp, &sts);
    uint64_t now; clock_get_uptime(&now);
    t0 = now - nsToAbs((uint64_t)((lp % ringBytes) / FRAME_BYTES) * 1000000000ULL / RATE);   // when frame 0 of this lap was written
    takeTimeStamp(false, &t0);
    lastLpib = lp % ringBytes; stallTicks = 0;
    running = true;
    timer->setTimeoutMS(tickMs);
    return true;
}

void OhMyDSPAudioEngine::bringDownLive()
{
    uint64_t a; clock_get_uptime(&a);
    DSPDevice::StopInfo si;
    bool ok = dsp->stopDma(&si);
    bool clean = dsp->freeStream();
    dsp->teardown(false);
    active = false;
    OLOG("engine stop: DMA stop %s, stream free %s, DSP down, %u ms", ok ? "OK" : "FAILED", clean ? "clean" : "NOT CLEAN", (unsigned)msSince(a));
}

// Health check reaction: tear the stream down, then restart it (at most MAX_RECOVERIES restarts per minute). If that does not work the engine
// keeps its clock running and delivers silence, so the application neither hangs nor sees an overload; the next start from the app tries again.
void OhMyDSPAudioEngine::recoverLive(const char *why)
{
    uint64_t now; clock_get_uptime(&now);
    if (absToNs(now - lastFault) > 60ULL * 1000000000ULL) faultCount = 0;
    lastFault = now; faults++;
    OLOG("FAULT: %s -> restarting the stream (restart %u of %u this minute, %llu faults in total)", why, (unsigned)(faultCount + 1), (unsigned)MAX_RECOVERIES, (unsigned long long)faults);
    bringDownLive();
    while (faultCount < MAX_RECOVERIES) {
        faultCount++;
        if (bringUpLive()) { recoveries++; OLOG("recovered after %s (%llu recoveries in total)", why, (unsigned long long)recoveries); return; }
        OLOG("restart attempt %u failed", (unsigned)faultCount);
    }
    memset(ringBase, 0, ringBytes);                 // DMA is down: nothing writes the ring any more
    failStarts = 0;                                 // fault injection must not leak refusals into the next application start
    silent = true; produced = 0; wraps = 0;
    clock_get_uptime(&t0);
    takeTimeStamp(false, &t0);
    OLOG("FAULT: giving up, the engine delivers silence until the application restarts the input");
}

IOReturn OhMyDSPAudioEngine::performAudioEngineStop()
{
    OLOG("engine stop (convert calls %llu, frames %llu)", (unsigned long long)convCalls, (unsigned long long)convFrames);
    running = false;
    timer->cancelTimeout();
    if (dsp && active) bringDownLive();
    return kIOReturnSuccess;
}

UInt64 OhMyDSPAudioEngine::framesNow() const
{
    uint64_t now; clock_get_uptime(&now);
    return absToNs(now - t0) * RATE / 1000000000ULL;
}

UInt32 OhMyDSPAudioEngine::getCurrentSampleFrame()
{
    gcsfCalls++;
    if (!running) return 0;
    if (dsp && !silent) { UInt32 lp = 0, sts = 0; dsp->sample(&lp, &sts); return (lp % ringBytes) / FRAME_BYTES; }
    return (UInt32)(framesNow() % ringFrames);
}

// The two microphones are one array: join them into a single signal and give it to both output channels. While a channel is digitally
// silent for the whole chunk (mic A has been exactly zero so far) the other one is used alone, so nothing is lost; as soon as both carry
// signal they are averaged. Software gain (live mode only) with a hard clip at full scale.
IOReturn OhMyDSPAudioEngine::convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *fmt, IOAudioStream *)
{
    if (!sampleBuf || !destBuf || !fmt || fmt->fNumChannels != 2 || fmt->fBitWidth != 32) return kIOReturnBadArgument;
    if ((UInt64)firstSampleFrame + numSampleFrames > ringFrames) return kIOReturnBadArgument;
    const SInt32 *src = (const SInt32 *)sampleBuf + (size_t)firstSampleFrame * 2;     // S32 LE, interleaved A, B
    float *dst = (float *)destBuf;                                                    // Float32 mix buffer, starts at the chunk
    bool aAny = false, bAny = false;
    UInt32 aN = 0, bN = 0, aP = 0, bP = 0;                                            // per-channel non-zero count and peak (log only)
    for (UInt32 i = 0; i < numSampleFrames; i++) {
        SInt32 a = src[2 * i], b = src[2 * i + 1];
        aN += (a != 0); bN += (b != 0);
        UInt32 ua = a < 0 ? (UInt32)(-(SInt64)a) : (UInt32)a, ub = b < 0 ? (UInt32)(-(SInt64)b) : (UInt32)b;
        if (ua > aP) aP = ua; if (ub > bP) bP = ub;
    }
    aAny = aN != 0; bAny = bN != 0;
    statANz += aN; statBNz += bN; if (aP > statAPeak) statAPeak = aP; if (bP > statBPeak) statBPeak = bP;
    const float k = (dsp ? gainLin : 1.0f) * (1.0f / 2147483648.0f);
    if (rawMode) {                                                                    // diagnostics: A on channel 0, B on channel 1, no join
        for (UInt32 i = 0; i < numSampleFrames; i++) {
            float l = (float)src[2 * i] * k, r = (float)src[2 * i + 1] * k;
            dst[2 * i] = l > 1.0f ? 1.0f : l < -1.0f ? -1.0f : l; dst[2 * i + 1] = r > 1.0f ? 1.0f : r < -1.0f ? -1.0f : r;
        }
        convCalls++; convFrames += numSampleFrames;
        return kIOReturnSuccess;
    }
    const float ka = (aAny && bAny) ? 0.5f * k : aAny ? k : 0.0f, kb = (aAny && bAny) ? 0.5f * k : bAny ? k : 0.0f;
    for (UInt32 i = 0; i < numSampleFrames; i++) {
        float m = (float)src[2 * i] * ka + (float)src[2 * i + 1] * kb;
        if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;
        dst[2 * i] = m; dst[2 * i + 1] = m;
    }
    convCalls++; convFrames += numSampleFrames;
    return kIOReturnSuccess;
}

IOReturn OhMyDSPAudioEngine::setProperties(OSObject *props)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, props);
    if (!d) return kIOReturnBadArgument;
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("tsmode"))) tsMode = n->unsigned32BitValue() ? 1 : 0;
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("tick_ms"))) { UInt32 v = n->unsigned32BitValue(); tickMs = v < 1 ? 1 : v > 20 ? 20 : v; }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("inject"))) {   // fault injection (live mode): 1 = stall the DMA, 2 = stall it and refuse the next 3 restarts
        UInt32 v = n->unsigned32BitValue(); if (dsp && active && !silent && (v == 1 || v == 2)) { if (v == 2) failStarts = 3; injectStall = true; OLOG("INJECT %u armed", (unsigned)v); }
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("raw"))) rawMode = n->unsigned32BitValue() != 0;   // 1 = no join: A left, B right
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("gain_db"))) { gainDbOverride = (SInt32)n->unsigned32BitValue(); applyGain(); }   // 1000 = back to the slider
    OLOG("tunables set: tsMode %u tickMs %u gainDbOverride %d (gain now x%d.%02d)", (unsigned)tsMode, (unsigned)tickMs, (int)gainDbOverride, (int)gainLin, (int)((gainLin - (int)gainLin) * 100));
    return kIOReturnSuccess;
}

void OhMyDSPAudioEngine::tickAction(OSObject *owner, IOTimerEventSource *)
{
    OhMyDSPAudioEngine *e = OSDynamicCast(OhMyDSPAudioEngine, owner);
    if (e) e->tick();
}

void OhMyDSPAudioEngine::tick()
{
    if (!running) return;
    ticks++;
    uint64_t n; clock_get_uptime(&n);
    if (n - lastStat > 2000000000ULL) {
        lastStat = n;
        if (gCfg.debug) OLOG("stat: ticks %llu wraps %llu getCurrentSampleFrame calls %llu convert calls %llu frames %llu lpib %u tsMode %u tickMs %u gain x%d",
             (unsigned long long)ticks, (unsigned long long)wraps, (unsigned long long)gcsfCalls, (unsigned long long)convCalls, (unsigned long long)convFrames,
             (unsigned)lastLpib, (unsigned)tsMode, (unsigned)tickMs, (int)gainLin);
        if (gCfg.debug) OLOG("chan: A non-zero %llu peak %u | B non-zero %llu peak %u (last 2 s, raw S32, before gain) raw=%d", (unsigned long long)statANz, (unsigned)statAPeak,
             (unsigned long long)statBNz, (unsigned)statBPeak, (int)rawMode);
        if (dsp && gCfg.debug) OLOG("health: faults %llu recoveries %llu silent %d", (unsigned long long)faults, (unsigned long long)recoveries, (int)silent);
        statANz = statBNz = 0; statAPeak = statBPeak = 0;
    }
    if (dsp && !silent) tickLive(); else tickSynthetic();
    timer->setTimeoutMS(tickMs);
}

void OhMyDSPAudioEngine::tickSynthetic()
{
    UInt64 total = framesNow();
    SInt32 *buf = (SInt32 *)ringBase;
    if (total - produced > ringFrames) produced = total - ringFrames;           // never fill more than one lap per tick
    for (; produced < total; produced++) {                                      // the "hardware" writes frames up to now
        SInt32 v = silent ? 0 : (SInt32)(((SInt64)kTone[produced % 48] * gain) / 100);
        UInt32 i = (UInt32)(produced % ringFrames) * 2;
        buf[i] = v; buf[i + 1] = v;
    }
    while (wraps < total / ringFrames) {                                        // one timestamp per buffer wrap, at the exact wrap time
        wraps++;
        uint64_t ns = (uint64_t)wraps * ringFrames * 1000000000ULL / RATE;
        AbsoluteTime ts = t0 + nsToAbs(ns);
        if (tsMode == 1) takeTimeStamp(true); else takeTimeStamp(true, &ts);
        if (gCfg.debug && (wraps <= 3 || wraps % 100 == 0)) OLOG("wrap %llu timestamp taken (ticks %llu)", (unsigned long long)wraps, (unsigned long long)ticks);
    }
}

// live: the DMA writes the ring by itself; we only read its position. LPIB is exact (the DMA clock is the DMIC clock), so a wrap is stamped
// at (now - time since the wrap), computed from the new LPIB, and no interrupt is needed. INTCTL / SD interrupt enables stay untouched.
void OhMyDSPAudioEngine::tickLive()
{
    UInt32 lp = 0, sts = 0; dsp->sample(&lp, &sts);
    lp %= ringBytes;
    if (injectStall) { injectStall = false; DSPDevice::StopInfo si; dsp->stopDma(&si); OLOG("INJECT: DMA stopped behind the engine's back"); return; }
    if (sts & 0x18) { char w[64]; snprintf(w, sizeof w, "SD error flags STS=0x%02x at LPIB %u", (unsigned)sts, (unsigned)lp); recoverLive(w); return; }
    if (lp == lastLpib) {
        if (++stallTicks >= STALL_TICKS) { char w[64]; snprintf(w, sizeof w, "LPIB stuck at %u for %u ms", (unsigned)lp, (unsigned)(STALL_TICKS * tickMs)); recoverLive(w); return; }
    } else stallTicks = 0;
    if (lp < lastLpib) {                                                         // the DMA wrapped to the start of the ring
        wraps++;
        uint64_t now; clock_get_uptime(&now);
        AbsoluteTime ts = now - nsToAbs((uint64_t)(lp / FRAME_BYTES) * 1000000000ULL / RATE);
        if (tsMode == 1) takeTimeStamp(true); else takeTimeStamp(true, &ts);
        if (gCfg.debug && (wraps <= 3 || wraps % 100 == 0)) OLOG("wrap %llu timestamp taken (lpib %u, ticks %llu)", (unsigned long long)wraps, (unsigned)lp, (unsigned long long)ticks);
    }
    lastLpib = lp;
}
