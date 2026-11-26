// Read-only. Every access below is a config read or an MMIO read of a status/ID register.
// Reference offsets: docs/bringup-recipe.md section 2 (sof-linux/sound/soc/sof/intel/hda.h).
#include "OhMyDSP.hpp"
#include <os/log.h>
#include <kern/thread_call.h>
#ifndef OHMYDSP_CORE
#include "MicAudio.hpp"
#else
#include "Capture.hpp"
#include "UserClient.hpp"
#endif
#include "DSPDevice.hpp"
#include "Config.hpp"
#include <pexpert/pexpert.h>
#include <kern/clock.h>

#define super IOService
OSDefineMetaClassAndStructors(OhMyDSP, IOService)

// Phase 1 output path. os_log/IOLog from this kext were not visible in the unified log (observed 2026-11-05),
// so every line is also appended to an IORegistry property "phase1-log" on this node:
//   ioreg -l -w0 -n OhMyDSP   (or: ioreg -rc OhMyDSP -w0)
#define DMIC_LOG_DECLARED 1
#define LOG(fmt, ...) do { os_log(OS_LOG_DEFAULT, "OhMyDSP: " fmt, ##__VA_ARGS__); record(fmt, ##__VA_ARGS__); } while (0)

void OhMyDSP::record(const char *fmt, ...)
{
    char line[300];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    IOLog("OhMyDSP: %s\n", line);
    if (stateLock) IOLockLock(stateLock);
    if (!logBuf) logBuf = OSArray::withCapacity(4096);
    // The IPC4 capture probe logs each mailbox word so a single NHLT module
    // init can consume hundreds of entries.  Keep enough history for the
    // complete run; if a future experiment exceeds the bound, expose that
    // fact in the registry instead of silently presenting a partial result.
    if (!logBuf || logBuf->getCount() >= 4096) {
        if (!logTruncated) {
            logTruncated = true;
            setProperty("phase1-log-truncated", true);
            IOLog("OhMyDSP: phase1 log capacity reached; subsequent lines are not retained\n");
        }
        if (stateLock) IOLockUnlock(stateLock);
        return;
    }
    OSString *s = OSString::withCString(line);
    if (s) { logBuf->setObject(s); s->release(); }
    if (stateLock) IOLockUnlock(stateLock);
    // NOTE: logBuf must never be handed to setProperty() while we still append to it (panics: "Trying to change a
    // collection in the registry"). It is published once, as a copy, by publishLog().
}

void OhMyDSP::dspLog(void *ctx, const char *fmt, ...)
{
    char line[300];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    static_cast<OhMyDSP *>(ctx)->record("%s", line);
}

static inline UInt32 rd(volatile UInt8 *b, size_t off) { return *(volatile UInt32 *)(b + off); }

// start() touches no hardware: it only schedules runProbe() ~30 s later, after AppleHDAController has finished initialising.
// (Boot hangs 2026-11-05 happened with probing in start(); see docs/2026-11-05-phase1-probe-boot-hang.md.)
static void probeEntry(thread_call_param_t p0, thread_call_param_t)
{
    OhMyDSP *self = (OhMyDSP *)p0;
    self->runProbe();
}

void OhMyDSP::publishLog()
{
    if (stateLock) IOLockLock(stateLock);
    OSArray *copy = logBuf ? OSArray::withArray(logBuf) : nullptr;
    UInt32 count = logBuf ? logBuf->getCount() : 0;
    bool truncated = logTruncated;
    if (stateLock) IOLockUnlock(stateLock);
    if (copy) { setProperty("phase1-log", copy); copy->release(); }
    setProperty("phase1-log-count", (unsigned long long)count, 32);
    setProperty("phase1-log-truncated", truncated);
}

bool OhMyDSP::claimClient(OhMyDSPUserClient *c)
{
    if (!stateLock) return false;
    IOLockLock(stateLock);
    bool available = !client;
    if (available) client = c;
    IOLockUnlock(stateLock);
    return available;
}

void OhMyDSP::releaseClient(OhMyDSPUserClient *c)
{
    if (!stateLock) return;
    IOLockLock(stateLock);
    if (client == c) client = nullptr;
    IOLockUnlock(stateLock);
}

void OhMyDSP::captureLog(const char *state, bool begin)
{
    if (begin) {
        IOLockLock(stateLock);
        if (logBuf) logBuf->release();
        logBuf = nullptr; logTruncated = false; sessionNumber++;
        IOLockUnlock(stateLock);
        removeProperty("ipc4-mtrace-stream");
    }
    LOG("IPC4 session %u: %s", sessionNumber, state);
    setProperty("ipc4-session-number", (unsigned long long)sessionNumber, 32);
    setProperty("ipc4-session-state", state);
    publishLog();
    if (!begin && dsp && dsp->ipc4TraceData())
        setProperty("ipc4-mtrace-stream", dsp->ipc4TraceData(), dsp->ipc4TraceSize());
    if (!begin) {
        char key[64];
        snprintf(key, sizeof(key), "ipc4-session-slot%u-log", sessionNumber % 4);
        OSObject *snapshot = copyProperty("phase1-log");
        if (snapshot) { setProperty(key, snapshot); snapshot->release(); }
        snprintf(key, sizeof(key), "ipc4-session-slot%u-number", sessionNumber % 4);
        setProperty(key, (unsigned long long)sessionNumber, 32);
        snprintf(key, sizeof(key), "ipc4-session-slot%u-trace", sessionNumber % 4);
        if (dsp && dsp->ipc4TraceData()) setProperty(key, dsp->ipc4TraceData(), dsp->ipc4TraceSize());
        else removeProperty(key);
    }
}

void OhMyDSP::runProbe()
{
    if (pciDev) runProbe(pciDev);
    if (pciDev) { pciDev->release(); pciDev = nullptr; }
    publishLog();
}

bool OhMyDSP::start(IOService *provider)
{
    stateLock = IOLockAlloc();
    if (!stateLock) return false;
    ohmydspLoadConfig();
    if (gCfg.off) { IOLog("OhMyDSP: -ohmydspoff set, not starting\n"); return false; }
    if (gLiluState == LiluRefused) { IOLog("OhMyDSP: Lilu refused the load (boot policy), not starting\n"); return false; }
    IOLog("OhMyDSP: start provider=%s\n", provider ? provider->getName() : "(null)");
    if (!super::start(provider)) { IOLog("OhMyDSP: super::start failed\n"); return false; }
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) { IOLog("OhMyDSP: provider is not IOPCIDevice, refusing\n"); return false; }
    tcall = thread_call_allocate(probeEntry, (thread_call_param_t)this);
    if (!tcall) return false;
    pci->retain(); pciDev = pci;
    uint64_t dl; clock_interval_to_deadline((uint32_t)gCfg.delay, NSEC_PER_SEC, &dl);   // seconds after start() before the probe runs (ohmydsp_delay)
    thread_call_enter_delayed(tcall, dl);
    registerService();
    IOLog("OhMyDSP: matched PCI provider and scheduled delayed probe\n");
    // Phase 5.1: audio device on a synthetic tone source (no DSP, no register access)
#if !defined(OHMYDSP_CORE) && !defined(OHMYDSP_RPL_EXPERIMENTAL)
    if (gCfg.audio == 1) {
        audioDev = new OhMyDSPAudioDevice;
        if (audioDev && audioDev->init(NULL) && audioDev->attach(this)) {
            if (!audioDev->start(this)) { IOLog("OhMyDSP: audio device start failed\n"); audioDev->detach(this); audioDev->release(); audioDev = nullptr; }
        } else if (audioDev) { audioDev->release(); audioDev = nullptr; }
    }
#endif
    return true;
}

void OhMyDSP::runProbe(IOPCIDevice *pci)
{
    LOG("probe running (delayed), provider %s", pci->getName());
    logConfig(pci);

    // Never power up or enable anything: bail out unless the function is already D0 with memory decode on.
    UInt16 cmd = pci->configRead16(0x04);
    // PM capability -> PMCSR
    UInt8 capPtr = pci->configRead8(0x34), pmcsr = 0xff;
    for (int i = 0; i < 48 && capPtr >= 0x40; i++) {
        UInt32 h = pci->configRead32(capPtr);
        if ((h & 0xff) == 0x01) { pmcsr = (UInt8)(pci->configRead16(capPtr + 4) & 3); break; }
        capPtr = (h >> 8) & 0xfc;
    }
    LOG("command=0x%04x (mem=%d busmaster=%d) PM state=D%d", cmd, (cmd >> 1) & 1, (cmd >> 2) & 1, pmcsr);
    if (!(cmd & 2) || pmcsr != 0) {
        LOG("function not in D0 / memory decode off: refusing to read BARs (would require a write to fix)");
        return;
    }

    // BAR0 = HDA regs, BAR4 = DSP regs. Mapping only; no register is written.
    IOMemoryMap *m0 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IOMemoryMap *m4 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (m0) { LOG("BAR0 phys=0x%llx len=0x%llx", (UInt64)m0->getPhysicalAddress(), (UInt64)m0->getLength());
              logHda((volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength()); }
    else LOG("BAR0 map failed");
    if (m4) { LOG("BAR4 phys=0x%llx len=0x%llx", (UInt64)m4->getPhysicalAddress(), (UInt64)m4->getLength());
              logDsp((volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength()); }
    else LOG("BAR4 map failed");

#ifdef OHMYDSP_RPL_EXPERIMENTAL
#ifdef OHMYDSP_RPL_IPC4_SESSION
    bool ready = m0 && m4 && startLiveAudio(pci, m0, m4);
    setProperty("ipc4-session-ready", ready);
    LOG("RPL IPC4 on-demand session %s; no boot-time recording", ready ? "ready" : "FAILED");
    if (m0) m0->release();
    if (m4) m4->release();
    return;
#endif
#ifdef OHMYDSP_RPL_LIVE
#ifdef OHMYDSP_RPL_MANUAL
    int rplLive = 1;
#else
    int rplLive = 0;
    PE_parse_boot_argn("ohmydsp_rpl_live", &rplLive, sizeof(rplLive));
#endif
    if (rplLive == 1 && m0 && m4) {
        // Discovery keeps the firmware-only boundary.  The probe build adds
        // one reviewed IPC4 round trip after the same mailbox dump.
#ifdef OHMYDSP_RPL_DISCOVER
        LOG("RPL LIVE discovery gate enabled: firmware boot plus IPC4 mailbox dump; pipeline disabled");
#elif defined(OHMYDSP_RPL_IPC4_PROBE)
        LOG("RPL LIVE IPC4 probe gate enabled: one pipeline-create round trip; capture disabled");
#elif defined(OHMYDSP_RPL_IPC4_MODULE_PROBE)
        LOG("RPL LIVE IPC4 module probe gate enabled: NHLT-backed module init; bind/capture disabled");
#elif defined(OHMYDSP_RPL_IPC4_BIND_PROBE)
        LOG("RPL LIVE IPC4 bind probe gate enabled: NHLT-backed module init and bind; capture disabled");
#elif defined(OHMYDSP_RPL_IPC4_CAPTURE_PROBE)
        LOG("RPL LIVE IPC4 capture probe gate enabled: NHLT-backed graph and pipeline-state probe; HDA DMA disabled");
#elif defined(OHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE)
        LOG("RPL LIVE IPC4 capture-DMA probe gate enabled: SD5 only, short HDA DMA, no IOAudio device");
#else
        LOG("RPL LIVE gate enabled: firmware-only boot attempt; IPC4 pipeline disabled");
#endif
        DSPDevice rpl(pci, (volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength(),
                      (volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength(), false, dspLog, this);
        bool ok = rpl.checkIdle() && rpl.loadFirmware();
        if (ok) ok = rpl.powerUp(
#if defined(OHMYDSP_RPL_DISCOVER) || defined(OHMYDSP_RPL_IPC4_PROBE) || defined(OHMYDSP_RPL_IPC4_MODULE_PROBE) || defined(OHMYDSP_RPL_IPC4_BIND_PROBE)
            true
#else
            false
#endif
        );
#ifdef OHMYDSP_RPL_IPC4_PROBE
        if (ok) ok = rpl.sendIpc4Probe();
#endif
#ifdef OHMYDSP_RPL_IPC4_MODULE_PROBE
        if (ok) ok = rpl.loadPipelineBlob();
        if (ok) ok = rpl.sendPipeline(9);
#endif
#ifdef OHMYDSP_RPL_IPC4_BIND_PROBE
        if (ok) ok = rpl.loadPipelineBlob();
        if (ok) ok = rpl.sendPipeline(15);
#endif
#ifdef OHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE
        bool ipc4Graph = false;
        bool ipc4Prepared = false;
        bool ipc4Trace = false;
        if (ok) ok = rpl.loadPipelineBlob();
        if (ok) ok = rpl.beginIpc4TraceCapture();
        if (ok) rpl.setIpc4TracePhase(1); // graph and initial PAUSED
        if (ok) { ipc4Graph = true; ok = rpl.sendIpc4CaptureGraph(5); }
        // Preserve run-9 transition order for this diagnostic experiment.
        // Initial PAUSED source-to-sink is NOT an exact Linux START match.
        // Descriptor setup leaves SD RUN clear until RUNNING succeeds.
        if (ok && ipc4Graph) ok = rpl.sendIpc4PipelineStateMulti(DSPDevice::IPC4_STATE_PAUSED, 0, 1, "IPC4 capture pipelines 12,11 PAUSED");
        if (ok && ipc4Graph) { ok = rpl.setupIpc4CaptureStream(5); ipc4Prepared = ok; }
        // The backend buffers from boot even before ENABLE_LOGS. The host
        // now consumes it throughout graph IPC; late enable arms notifications.
        if (ok && ipc4Prepared) {
            LOG("IPC4 capture: observing SD5 for 1000 ms");
            rpl.setIpc4TracePhase(2); // enable log notifications
            ipc4Trace = true; // Disable even if the enable reply timed out.
            ok = rpl.setIpc4Trace(true);
        }
        // SOF sends IPC4 RUNNING first; only after that succeeds does the
        // platform trigger HDA DMA.  Keep that ordering explicit so an
        // invalid RUNNING transition cannot leave an active stream behind.
        if (ok && ipc4Prepared) {
            rpl.setIpc4TracePhase(3); // RUNNING and optional DMA observation
            ok = rpl.sendIpc4PipelineStateMulti(DSPDevice::IPC4_STATE_RUNNING, 1, 0, "IPC4 capture pipelines 11,12 RUNNING");
        }
        if (ok && ipc4Prepared) ok = rpl.startIpc4CaptureDma();
        if (ok && ipc4Prepared) ok = rpl.observeIpc4CaptureStream(1000);
        bool clean = rpl.finishIpc4CaptureProbe(ipc4Graph, ipc4Trace);
        if (rpl.ipc4TraceData())
            setProperty("ipc4-mtrace-stream", rpl.ipc4TraceData(), rpl.ipc4TraceSize());
        ok = ok && clean;
#endif
#ifdef OHMYDSP_RPL_IPC4_CAPTURE_PROBE
        if (ok) ok = rpl.loadPipelineBlob();
        if (ok) ok = rpl.sendPipeline(15);
        // IPC4 starts the host-facing pipeline first, then the DMIC pipeline.
        // This state-only pass exercises graph preparation without programming
        // an HDA stream or starting capture DMA.
        if (ok) ok = rpl.sendIpc4PipelineState(1, 3, "IPC4 pipeline 11 PAUSED");
        if (ok) ok = rpl.sendIpc4PipelineState(0, 3, "IPC4 pipeline 12 PAUSED");
        if (ok) ok = rpl.sendIpc4PipelineState(0, 2, "IPC4 pipeline 12 RESET");
        if (ok) ok = rpl.sendIpc4PipelineState(1, 2, "IPC4 pipeline 11 RESET");
#endif
#ifndef OHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE
        rpl.teardown(false);
#endif
        LOG("RPL LIVE result: %s", ok ? "PASS" : "FAILED");
        if (m0) m0->release();
        if (m4) m4->release();
        LOG("probe done (RPL %s)",
#ifdef OHMYDSP_RPL_DISCOVER
            "IPC4 mailbox discovery"
#elif defined(OHMYDSP_RPL_IPC4_PROBE)
            "IPC4 transport probe"
#elif defined(OHMYDSP_RPL_IPC4_MODULE_PROBE)
            "IPC4 NHLT-backed module probe"
#elif defined(OHMYDSP_RPL_IPC4_BIND_PROBE)
            "IPC4 NHLT-backed bind probe"
#elif defined(OHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE)
            "IPC4 capture DMA probe"
#elif defined(OHMYDSP_RPL_IPC4_CAPTURE_PROBE)
            "IPC4 capture pipeline-state probe"
#else
            "firmware-only"
#endif
        );
        return;
    }
    if (rplLive == 1) LOG("RPL LIVE requested but BAR mapping is incomplete; refusing firmware boot");
#endif
    // The first Raptor Lake build is deliberately read-only. It validates PCI/BAR
    // visibility and captures diagnostics, but never starts firmware, IPC, DMA,
    // an IOAudioFamily device, or a user-client capture session.
    LOG("RPL experimental build: read-only probe complete; firmware/IPC/DMA/audio disabled");
    if (m0) m0->release();
    if (m4) m4->release();
    LOG("probe done (RPL read-only)");
    return;
#endif

    // Phase 5.3 (boot-arg ohmydsp_audio=2): live microphones. The audio engine owns the DSP (power-up on engine start), the Phase 2-4 harness is not used.
    // Phase 2 (DSP power-up + firmware load) only when explicitly requested by boot-arg; see docs/phase2-plan.md.
    const int au = gCfg.audio;
    int p2 = 0; PE_parse_boot_argn("ohmydsp_p2", &p2, sizeof(p2));
    bool kept = false;
    if (au == 2) {
        if (p2) LOG("ohmydsp_audio=2 and ohmydsp_p2 both set: the audio engine owns the DSP, the Phase 2-4 harness is skipped");
        if (m0 && m4) kept = startLiveAudio(pci, m0, m4);
        else LOG("Live audio requested but a BAR map is missing: not running");
    } else if (p2 == 1 || p2 == 2) {
        if (m0 && m4) bootDSP(pci, (volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength(),
                              (volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength(), p2 == 2);
        else LOG("Phase 2 requested but a BAR map is missing: not running");
    } else LOG("Phase 2 disabled (boot-arg ohmydsp_p2 not set)");
    if (!kept) { if (m0) m0->release(); if (m4) m4->release(); }   // kept maps are released in stop()

    LOG("probe done");
    return;
}

// Prepare the DSPDevice once (software only: register READS and buffer allocation, no register write), then publish the audio device.
// The DSP itself is powered up only when an application starts the input (MicAudioEngine::performAudioEngineStart).
#ifdef OHMYDSP_CORE
// CORE variant: prepare the DSPDevice once (software only: register READS and buffer allocation, no register write), then create the capture
// session that the user client talks to. The DSP itself is powered up only when the client starts the stream.
bool OhMyDSP::startLiveAudio(IOPCIDevice *pci, IOMemoryMap *m0, IOMemoryMap *m4)
{
    const UInt32 LIVE_RING_BYTES =
#ifdef OHMYDSP_RPL_IPC4_SESSION
        DSPDevice::CAPTURE_BYTES;
#else
        (UInt32)gCfg.ringKiB * 1024;
#endif
    DSPDevice *d = new DSPDevice(pci, (volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength(),
                                 (volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength(), false, dspLog, this);
    if (!d) return false;
    if (!(d->checkIdle() && d->setCaptureBytes(LIVE_RING_BYTES) && d->loadPipelineBlob() && d->loadFirmware() && d->allocStreamBuffers())) {
        LOG("live capture: preparation failed (see the lines above), no capture session"); delete d; return false; }
    OhMyDSPCapture *c = new OhMyDSPCapture;
    if (!c || !c->setup(d, this)) { LOG("live capture: session setup failed"); if (c) c->release(); d->releaseStreamBuffers(); delete d; return false; }
    dsp = d; cap = c; m0->retain(); m4->retain(); map0 = m0; map4 = m4;
    LOG("live capture ready: DSP idle, powers up when the user client starts the stream");
    return true;
}
#else
bool OhMyDSP::startLiveAudio(IOPCIDevice *pci, IOMemoryMap *m0, IOMemoryMap *m4)
{
    const UInt32 LIVE_RING_BYTES = (UInt32)gCfg.ringKiB * 1024;   // default 128 KiB = 16384 frames = 0.34 s (proven with the synthetic tone in 5.1)
    DSPDevice *d = new DSPDevice(pci, (volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength(),
                                 (volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength(), false, dspLog, this);
    if (!d) return false;
    if (!(d->checkIdle() && d->setCaptureBytes(LIVE_RING_BYTES) && d->loadPipelineBlob() && d->loadFirmware() && d->allocStreamBuffers())) {
        LOG("live audio: preparation failed (see the lines above), no audio device"); delete d; return false; }
    OhMyDSPAudioDevice *dev = new OhMyDSPAudioDevice;
    if (!dev) { delete d; return false; }
    dev->dsp = d;
    if (!dev->init(NULL) || !dev->attach(this)) { LOG("live audio: audio device init failed"); dev->release(); delete d; return false; }
    if (!dev->start(this)) { LOG("live audio: audio device start failed"); dev->detach(this); dev->release(); delete d; return false; }
    dsp = d; audioDev = dev; m0->retain(); m4->retain(); map0 = m0; map4 = m4;
    LOG("live audio ready: DSP idle, powers up when an app starts the input");
    return true;
}
#endif

void OhMyDSP::free()
{
    if (stateLock) { IOLockFree(stateLock); stateLock = nullptr; }
    super::free();
}

void OhMyDSP::stop(IOService *provider)
{
    if (tcall) {
        thread_call_cancel(tcall);                         // public KPI only (kpi.private is not allowed for third-party kexts)
        while (!thread_call_free(tcall)) IOSleep(10);      // FALSE while the probe is still running: wait for it to finish
        tcall = nullptr;
    }
#ifdef OHMYDSP_CORE
    if (cap) { cap->shutdown(); cap->release(); cap = nullptr; }   // stops a running stream (DSP teardown) first
#else
    if (audioDev) { audioDev->terminate(kIOServiceSynchronous); audioDev->release(); audioDev = nullptr; }   // stops the engine (DSP teardown) first
#endif
    if (dsp) {   // safety net: idempotent, writes nothing when the engine already tore everything down
        dsp->freeStream(); dsp->teardown(false); dsp->releaseStreamBuffers();
        delete dsp; dsp = nullptr;
    }
    if (map0) { map0->release(); map0 = nullptr; }
    if (map4) { map4->release(); map4 = nullptr; }
    if (pciDev) { pciDev->release(); pciDev = nullptr; }
    if (logBuf) { logBuf->release(); logBuf = nullptr; }
    super::stop(provider);
}

void OhMyDSP::logConfig(IOPCIDevice *pci)
{
    for (int off = 0; off < 0x100; off += 16)
        LOG("cfg[0x%02x] %08x %08x %08x %08x", off, pci->configRead32(off), pci->configRead32(off + 4),
            pci->configRead32(off + 8), pci->configRead32(off + 12));
    LOG("PGCTL(0x44)=%08x CGCTL(0x48)=%08x", pci->configRead32(0x44), pci->configRead32(0x48));
}

void OhMyDSP::logHda(volatile UInt8 *b, size_t len)
{
    if (!b || len < 0x1000) return;
    UInt32 gcap = *(volatile UInt16 *)(b + 0x00);
    unsigned iss = (gcap >> 8) & 0xf, oss = (gcap >> 12) & 0xf, bss = (gcap >> 3) & 0x1f;
    LOG("HDA GCAP=%04x (in=%u out=%u bidir=%u) GCTL=%08x WAKESTS=%04x INTCTL=%08x INTSTS=%08x LLCH=%08x",
        gcap, iss, oss, bss, rd(b, 0x08), *(volatile UInt16 *)(b + 0x0e), rd(b, 0x20), rd(b, 0x24), rd(b, 0x14));
    // Stream descriptors: inputs (capture) first, then outputs. Shows which ones AppleHDA programmed.
    unsigned n = iss + oss + bss;
    if (n > 30) n = 30;
    for (unsigned i = 0; i < n; i++) {
        size_t o = 0x80 + 0x20 * i;
        if (o + 0x20 > len) break;
        UInt32 ctl = rd(b, o);
        LOG("SD%u(%s) CTL/STS=%08x tag=%u run=%u LPIB=%08x CBL=%08x LVI/FIFOW=%08x FIFOS/FMT=%08x BDL=%08x:%08x",
            i, i < iss ? "in" : (i < iss + oss ? "out" : "bidir"), ctl, (ctl >> 20) & 0xf, (ctl >> 1) & 1,
            rd(b, o + 4), rd(b, o + 8), rd(b, o + 0xc), rd(b, o + 0x10), rd(b, o + 0x1c), rd(b, o + 0x18));
    }
    // Capability chain (pp=3 spib=4 drsm=5), same walk as hda_dsp_ctrl_get_caps(); bounded.
    UInt32 off = rd(b, 0x14) & 0xffff;
    for (int i = 0; i < 16 && off && (off & 3) == 0 && off + 8 <= len; i++) {
        UInt32 cap = rd(b, off);
        if (cap == 0xffffffff) { LOG("cap read all-ones at 0x%x", off); break; }
        LOG("cap@0x%04x id=%u hdr=%08x +4=%08x +8=%08x", off, (cap >> 16) & 0xfff, cap, rd(b, off + 4), rd(b, off + 8));
        off = cap & 0xffff;
    }
    LOG("VS EM2(0x1030)=%08x LTRP(0x1048)=%08x D0I3C(0x104a)=%04x", rd(b, 0x1030), rd(b, 0x1048),
        *(volatile UInt16 *)(b + 0x104a));
}

void OhMyDSP::logDsp(volatile UInt8 *b, size_t len)
{
    if (!b || len < 0x90000) { LOG("BAR4 too small (0x%zx) for ROM status window", len); return; }
    UInt32 adspcs = rd(b, 0x04);
    LOG("DSP ADSPCS=%08x ADSPIC=%08x ADSPIS=%08x", adspcs, rd(b, 0x08), rd(b, 0x0c));
    LOG("DSP HIPCTDR=%08x HIPCTDA=%08x HIPCTDD=%08x HIPCIDR=%08x HIPCIDA=%08x HIPCIDD=%08x HIPCCTL=%08x",
        rd(b, 0xc0), rd(b, 0xc4), rd(b, 0xc8), rd(b, 0xd0), rd(b, 0xd4), rd(b, 0xd8), rd(b, 0xe8));
    // The SRAM window is only safe to read while DSP core 0 is powered (ADSPCS.CPA bit24; SOF marks it D0-only).
    // A read while the DSP is power-gated can stall the bus, so skip it unless CPA is set. (Phase 1 hang, 2026-11-05.)
    if (adspcs != 0xffffffff && ((adspcs >> 24) & 1))
        LOG("DSP ROM_STATUS(0x80000)=%08x ROM_ERROR/FW_STATUS(0x80004)=%08x", rd(b, 0x80000), rd(b, 0x80004));
    else
        LOG("DSP core0 not powered (ADSPCS=%08x): SRAM window NOT read", adspcs);
}
