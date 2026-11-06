// Phase 6.1: see Capture.hpp. Logic carried over unchanged from OhMyDSPAudioEngine (MicAudio.cpp): same log texts ("OhMyDSP audio: ..."),
// same latency line, same recovery policy (stall 200 ms or SD error -> restart, 3 per minute, then silence on a software clock).
#include "Capture.hpp"
#include "DSPDevice.hpp"
#include "Config.hpp"
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>

#define OLOG(fmt, ...) do { os_log(OS_LOG_DEFAULT, "OhMyDSP audio: " fmt, ##__VA_ARGS__); } while (0)

enum : UInt32 { RATE = 48000, FRAME_BYTES = 8, LIVE_PCM_MSGS = 19, STALL_TICKS = 40, MAX_RECOVERIES = 3 };
static inline uint64_t nsToAbs(uint64_t ns) { uint64_t a = 0; nanoseconds_to_absolutetime(ns, &a); return a; }
static inline uint64_t absToNs(uint64_t a) { uint64_t n = 0; absolutetime_to_nanoseconds(a, &n); return n; }
static inline uint32_t msSince(uint64_t from) { uint64_t n; clock_get_uptime(&n); return (uint32_t)(absToNs(n - from) / 1000000ULL); }

#define super OSObject
OSDefineMetaClassAndStructors(OhMyDSPCapture, OSObject)
#undef super

bool OhMyDSPCapture::setup(DSPDevice *d)
{
    if (!OSObject::init()) return false;
    dsp = d;
    ringBytes = dsp->captureBytes(); ringFrames = ringBytes / FRAME_BYTES;
    tickMs = (UInt32)gCfg.tickMs;
    wl = IOWorkLoop::workLoop();
    if (!wl) return false;
    gate = IOCommandGate::commandGate(this);
    timer = IOTimerEventSource::timerEventSource(this, tickAction);
    statusBuf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, 4096, page_size);
    if (!gate || !timer || !statusBuf || wl->addEventSource(gate) != kIOReturnSuccess || wl->addEventSource(timer) != kIOReturnSuccess) { shutdown(); return false; }
    st = (OhMyDSPStatus *)statusBuf->getBytesNoCopy();
    memset(st, 0, 4096);
    st->magic = OHMYDSP_STATUS_MAGIC; st->version = OHMYDSP_STATUS_VERSION;
    st->ringBytes = ringBytes; st->frameBytes = FRAME_BYTES; st->rate = RATE; st->channels = 2;
    OLOG("capture ready: %u Hz, 2 ch S32, %u frames ring (%u bytes)", (unsigned)RATE, (unsigned)ringFrames, (unsigned)ringBytes);
    return true;
}

void OhMyDSPCapture::shutdown()
{
    if (gate && wl) gate->runAction(gateStop, this);     // stops the stream on the loop thread
    if (timer) { timer->cancelTimeout(); if (wl) wl->removeEventSource(timer); timer->release(); timer = nullptr; }
    if (gate) { if (wl) wl->removeEventSource(gate); gate->release(); gate = nullptr; }
    if (wl) { wl->release(); wl = nullptr; }
    if (statusBuf) { statusBuf->release(); statusBuf = nullptr; st = nullptr; }
}

IOMemoryDescriptor *OhMyDSPCapture::ringMemory() const { return dsp->captureMemory(); }

IOReturn OhMyDSPCapture::start() { return gate ? gate->runAction(gateStart, this) : kIOReturnNotReady; }
IOReturn OhMyDSPCapture::stop()  { return gate ? gate->runAction(gateStop, this) : kIOReturnNotReady; }
IOReturn OhMyDSPCapture::tune(uint32_t key, uint64_t value) { return gate ? gate->runAction(gateTune, (void *)(uintptr_t)key, (void *)(uintptr_t)value) : kIOReturnNotReady; }
IOReturn OhMyDSPCapture::gateStart(OSObject *o, void *, void *, void *, void *) { return static_cast<OhMyDSPCapture *>(o)->doStart(); }
IOReturn OhMyDSPCapture::gateStop(OSObject *o, void *, void *, void *, void *) { return static_cast<OhMyDSPCapture *>(o)->doStop(); }
IOReturn OhMyDSPCapture::gateTune(OSObject *o, void *a, void *b, void *, void *)
{
    OhMyDSPCapture *c = static_cast<OhMyDSPCapture *>(o);
    uint32_t key = (uint32_t)(uintptr_t)a, v = (uint32_t)(uintptr_t)b;
    if (key == OHMYDSP_TUNE_INJECT && c->active && !c->silent && (v == 1 || v == 2)) {
        if (v == 2) c->failStarts = 3;
        c->injectStall = true; OLOG("INJECT %u armed", (unsigned)v); return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

IOReturn OhMyDSPCapture::doStart()
{
    if (running) return kIOReturnSuccess;
    OLOG("engine start (live)");
    silent = false; stallTicks = 0; lastLpib = 0; laps = 0;
    if (!bringUp(false)) return kIOReturnError;
    return kIOReturnSuccess;
}

IOReturn OhMyDSPCapture::doStop()
{
    if (!running && !active) return kIOReturnSuccess;
    OLOG("engine stop");
    running = false;
    if (timer) timer->cancelTimeout();
    if (active) bringDown();
    publish(OHMYDSP_STATE_STOPPED, 0);
    return kIOReturnSuccess;
}

void OhMyDSPCapture::publish(uint32_t state, uint32_t lpib) { if (st) { st->lpib = lpib; st->state = state; } }
void OhMyDSPCapture::publishLap(uint64_t l, uint64_t hostTime)
{
    if (!st) return;
    st->seq++; __sync_synchronize();
    st->laps = l; st->lapHostTime = hostTime;
    __sync_synchronize(); st->seq++;
}

// DSP power-up + firmware + pipeline + capture stream (all steps logged and bounded by DSPDevice). Used by start and by fault recovery.
// After a system wake AppleHDAController may still be bringing the controller back, so the idle check is retried a few times (bounded, 0.8 s).
bool OhMyDSPCapture::bringUp(bool restart)
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
    if (!ok) { dsp->freeStream(); dsp->teardown(false); return false; }
    active = true;
    UInt32 lp = 0, sts = 0; dsp->sample(&lp, &sts);
    uint64_t now; clock_get_uptime(&now);
    t0 = now - nsToAbs((uint64_t)((lp % ringBytes) / FRAME_BYTES) * 1000000000ULL / RATE);   // when frame 0 of this lap was written
    if (restart) laps++;                            // keep the sample timeline monotonic across a recovery
    publishLap(laps, t0);
    if (st) st->epoch++;
    lastLpib = lp % ringBytes; stallTicks = 0;
    publish(OHMYDSP_STATE_RUNNING, lastLpib);
    running = true;
    timer->setTimeoutMS(tickMs);
    return true;
}

void OhMyDSPCapture::bringDown()
{
    uint64_t a; clock_get_uptime(&a);
    DSPDevice::StopInfo si;
    bool ok = dsp->stopDma(&si);
    bool clean = dsp->freeStream();
    dsp->teardown(false);
    active = false;
    OLOG("engine stop: DMA stop %s, stream free %s, DSP down, %u ms", ok ? "OK" : "FAILED", clean ? "clean" : "NOT CLEAN", (unsigned)msSince(a));
}

// Health check reaction: tear the stream down, then restart it (at most MAX_RECOVERIES restarts per minute). If that does not work the session
// keeps its clock running and publishes silence, so the client neither hangs nor sees an overload; the next start from the client tries again.
void OhMyDSPCapture::recover(const char *why)
{
    uint64_t now; clock_get_uptime(&now);
    if (absToNs(now - lastFault) > 60ULL * 1000000000ULL) faultCount = 0;
    lastFault = now; faults++; if (st) st->faults = faults;
    OLOG("FAULT: %s -> restarting the stream (restart %u of %u this minute, %llu faults in total)", why, (unsigned)(faultCount + 1), (unsigned)MAX_RECOVERIES, (unsigned long long)faults);
    bringDown();
    while (faultCount < MAX_RECOVERIES) {
        faultCount++;
        if (bringUp(true)) { recoveries++; if (st) st->recoveries = recoveries; OLOG("recovered after %s (%llu recoveries in total)", why, (unsigned long long)recoveries); return; }
        OLOG("restart attempt %u failed", (unsigned)faultCount);
    }
    memset(dsp->captureBase(), 0, ringBytes);       // DMA is down: nothing writes the ring any more
    failStarts = 0;                                 // fault injection must not leak refusals into the next start
    silent = true; laps = 0;
    clock_get_uptime(&t0);
    publishLap(0, t0);
    publish(OHMYDSP_STATE_SILENT, 0);
    OLOG("FAULT: giving up, the session publishes silence until the client restarts the input");
}

void OhMyDSPCapture::tickAction(OSObject *owner, IOTimerEventSource *)
{
    OhMyDSPCapture *c = OSDynamicCast(OhMyDSPCapture, owner);
    if (c) c->tick();
}

void OhMyDSPCapture::tick()
{
    if (!running) return;
    ticks++;
    uint64_t n; clock_get_uptime(&n);
    if (n - lastStat > 2000000000ULL) {
        lastStat = n;
        if (gCfg.debug) OLOG("health: ticks %llu laps %llu lpib %u faults %llu recoveries %llu silent %d", (unsigned long long)ticks, (unsigned long long)laps, (unsigned)lastLpib,
                             (unsigned long long)faults, (unsigned long long)recoveries, (int)silent);
    }
    if (silent) tickSilent(); else tickLive();
    if (running) timer->setTimeoutMS(tickMs);
}

// live: the DMA writes the ring by itself; we only read its position. LPIB is exact (the DMA clock is the DMIC clock), so a wrap is stamped
// at (now - time since the wrap), computed from the new LPIB, and no interrupt is needed. INTCTL / SD interrupt enables stay untouched.
void OhMyDSPCapture::tickLive()
{
    UInt32 lp = 0, sts = 0; dsp->sample(&lp, &sts);
    lp %= ringBytes;
    if (injectStall) { injectStall = false; DSPDevice::StopInfo si; dsp->stopDma(&si); OLOG("INJECT: DMA stopped behind the engine's back"); return; }
    if (sts & 0x18) { char w[64]; snprintf(w, sizeof w, "SD error flags STS=0x%02x at LPIB %u", (unsigned)sts, (unsigned)lp); recover(w); return; }
    if (lp == lastLpib) {
        if (++stallTicks >= STALL_TICKS) { char w[64]; snprintf(w, sizeof w, "LPIB stuck at %u for %u ms", (unsigned)lp, (unsigned)(STALL_TICKS * tickMs)); recover(w); return; }
    } else stallTicks = 0;
    if (lp < lastLpib) {                                                         // the DMA wrapped to the start of the ring
        laps++;
        uint64_t now; clock_get_uptime(&now);
        publishLap(laps, now - nsToAbs((uint64_t)(lp / FRAME_BYTES) * 1000000000ULL / RATE));
        if (gCfg.debug && (laps <= 3 || laps % 100 == 0)) OLOG("wrap %llu published (lpib %u, ticks %llu)", (unsigned long long)laps, (unsigned)lp, (unsigned long long)ticks);
    }
    lastLpib = lp;
    if (st) st->lpib = lp;
}

// silent: no DMA any more; the position follows a software clock so clients keep running at the right speed (the ring is all zeros).
void OhMyDSPCapture::tickSilent()
{
    uint64_t now; clock_get_uptime(&now);
    uint64_t total = absToNs(now - t0) * RATE / 1000000000ULL;
    uint64_t l = total / ringFrames;
    if (l != laps) { laps = l; publishLap(laps, t0 + nsToAbs(l * ringFrames * 1000000000ULL / RATE)); }
    lastLpib = (UInt32)(total % ringFrames) * FRAME_BYTES;
    if (st) st->lpib = lastLpib;
}
