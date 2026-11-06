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
    if (!logBuf) logBuf = OSArray::withCapacity(256);
    if (!logBuf || logBuf->getCount() >= 1000) return;
    OSString *s = OSString::withCString(line);
    if (s) { logBuf->setObject(s); s->release(); }
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
    if (!logBuf) return;
    OSArray *copy = OSArray::withArray(logBuf);
    if (copy) { setProperty("phase1-log", copy); copy->release(); }
}

void OhMyDSP::runProbe()
{
    if (pciDev) runProbe(pciDev);
    if (pciDev) { pciDev->release(); pciDev = nullptr; }
    publishLog();
}

bool OhMyDSP::start(IOService *provider)
{
    ohmydspLoadConfig();
    if (gCfg.off) { IOLog("OhMyDSP: -ohmydspoff set, not starting\n"); return false; }
    if (gLiluState == LiluRefused) { IOLog("OhMyDSP: Lilu refused the load (boot policy), not starting\n"); return false; }
    if (!super::start(provider)) return false;
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    tcall = thread_call_allocate(probeEntry, (thread_call_param_t)this);
    if (!tcall) return false;
    pci->retain(); pciDev = pci;
    uint64_t dl; clock_interval_to_deadline((uint32_t)gCfg.delay, NSEC_PER_SEC, &dl);   // seconds after start() before the probe runs (ohmydsp_delay)
    thread_call_enter_delayed(tcall, dl);
    registerService();
    // Phase 5.1: audio device on a synthetic tone source (no DSP, no register access)
#ifndef OHMYDSP_CORE
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
    const UInt32 LIVE_RING_BYTES = (UInt32)gCfg.ringKiB * 1024;
    DSPDevice *d = new DSPDevice(pci, (volatile UInt8 *)m0->getVirtualAddress(), (size_t)m0->getLength(),
                                 (volatile UInt8 *)m4->getVirtualAddress(), (size_t)m4->getLength(), false, dspLog, this);
    if (!d) return false;
    if (!(d->checkIdle() && d->setCaptureBytes(LIVE_RING_BYTES) && d->loadPipelineBlob() && d->loadFirmware() && d->allocStreamBuffers())) {
        LOG("live capture: preparation failed (see the lines above), no capture session"); delete d; return false; }
    OhMyDSPCapture *c = new OhMyDSPCapture;
    if (!c || !c->setup(d)) { LOG("live capture: session setup failed"); if (c) c->release(); d->releaseStreamBuffers(); delete d; return false; }
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
