// Phase 6.1: the capture session without IOAudioFamily. Owns the "engine" logic of MicAudio.cpp (DSP bring-up, health check, recovery,
// silent fallback) but publishes the result through a shared status page + the DMA ring instead of IOAudioEngine calls, so the kext
// can live in the OpenCore-injected Boot KC. Audio conversion (mic join, gain) moves to userspace.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "../include/OhMyDSPShared.h"

class DSPDevice;

class OhMyDSPCapture : public OSObject {
    OSDeclareDefaultStructors(OhMyDSPCapture)
public:
    bool setup(DSPDevice *d);                   // work loop, timer, gate, status page (software only)
    void shutdown();                            // stops a running stream, releases everything (idempotent)
    IOReturn start();                           // may be called from any thread; serialised on the capture work loop
    IOReturn stop();
    IOReturn tune(uint32_t key, uint64_t value);
    IOMemoryDescriptor *ringMemory() const;
    IOMemoryDescriptor *statusMemory() const { return statusBuf; }
private:
    static IOReturn gateStart(OSObject *o, void *, void *, void *, void *);
    static IOReturn gateStop(OSObject *o, void *, void *, void *, void *);
    static IOReturn gateTune(OSObject *o, void *a, void *, void *, void *);
    static void tickAction(OSObject *owner, IOTimerEventSource *sender);
    IOReturn doStart(); IOReturn doStop(); void tick();
    void tickLive(); void tickSilent();
    bool bringUp(bool restart); void bringDown(); void recover(const char *why);
    void publish(uint32_t state, uint32_t lpib);            // status page: state + position
    void publishLap(uint64_t laps, uint64_t hostTime);      // status page: seqlock-protected lap counter + time
    DSPDevice *dsp = nullptr;
    IOWorkLoop *wl = nullptr; IOCommandGate *gate = nullptr; IOTimerEventSource *timer = nullptr;
    IOBufferMemoryDescriptor *statusBuf = nullptr; OhMyDSPStatus *st = nullptr;
    UInt32 ringBytes = 0, ringFrames = 0;
    bool running = false, active = false, silent = false, injectStall = false;
    int failStarts = 0;
    UInt32 lastLpib = 0, stallTicks = 0, faultCount = 0, tickMs = 5;
    uint64_t t0 = 0, lastFault = 0, laps = 0;       // t0: host time of frame 0 of the current run (silent mode clock)
    UInt64 faults = 0, recoveries = 0, ticks = 0, lastStat = 0;
};
