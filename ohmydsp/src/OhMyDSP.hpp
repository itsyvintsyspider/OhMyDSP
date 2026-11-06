// Phase 1: READ-ONLY probe of the Intel HDA/DSP PCI function (8086:06c8).
// Never opens the provider, never enables memory/bus-master, never writes config or MMIO.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <stdarg.h>
#include <kern/thread_call.h>

class OhMyDSPAudioDevice;
class OhMyDSPCapture;
class OhMyDSPUserClient;
class DSPDevice;
class IOMemoryMap;

class OhMyDSP : public IOService {
    OSDeclareDefaultStructors(OhMyDSP)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void runProbe();
    void publishLog();
    OhMyDSPCapture *capture() const { return cap; }          // CORE variant: the capture session behind the user client (null until the DSP is prepared)
    bool claimClient(OhMyDSPUserClient *c) { if (client) return false; client = c; return true; }
    void releaseClient(OhMyDSPUserClient *c) { if (client == c) client = nullptr; }
    static void dspLog(void *ctx, const char *fmt, ...);   // log sink for DSPDevice (same path as record())
private:
    void record(const char *fmt, ...) __printflike(2, 3);
    OSArray *logBuf = nullptr;
    thread_call_t tcall = nullptr;
    IOPCIDevice *pciDev = nullptr;
    OhMyDSPAudioDevice *audioDev = nullptr;   // Phase 5 audio device (boot-arg ohmydsp_audio=1 synthetic tone, =2 live DSP microphones)
    OhMyDSPCapture *cap = nullptr; OhMyDSPUserClient *client = nullptr;   // CORE variant (no IOAudioFamily)
    DSPDevice *dsp = nullptr;                          // live mode: owned here, used by the audio engine
    IOMemoryMap *map0 = nullptr, *map4 = nullptr;      // live mode: BAR0 / BAR4 mappings the DSPDevice keeps using
    bool startLiveAudio(IOPCIDevice *pci, IOMemoryMap *m0, IOMemoryMap *m4);
    void runProbe(IOPCIDevice *pci);
    bool bootDSP(IOPCIDevice *pci, volatile UInt8 *b0, size_t len0, volatile UInt8 *b4, size_t len4, bool dryRun);
    void logConfig(IOPCIDevice *pci);
    void logHda(volatile UInt8 *base, size_t len);
    void logDsp(volatile UInt8 *base, size_t len);
};
