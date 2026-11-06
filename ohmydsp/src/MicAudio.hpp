// Phase 5: the macOS audio side (IOAudioFamily). Two modes of the same engine:
//   dsp == nullptr : 5.1 synthetic 1 kHz tone (no DSP, no register access, no DMA)            boot-arg ohmydsp_audio=1
//   dsp != nullptr : 5.3 live microphones: engine start powers the DSP and starts the DMA, the engine's sample buffer IS the
//                    DMA ring, a timer polls LPIB and stamps every wrap, stop tears everything down      boot-arg ohmydsp_audio=2
#pragma once
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioSelectorControl.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>

class DSPDevice;

class OhMyDSPAudioDevice : public IOAudioDevice {
    OSDeclareDefaultStructors(OhMyDSPAudioDevice)
public:
    bool initHardware(IOService *provider) override;
    DSPDevice *dsp = nullptr;               // set by the provider before start(); owned by the provider
};

class OhMyDSPAudioEngine : public IOAudioEngine {
    OSDeclareDefaultStructors(OhMyDSPAudioEngine)
public:
    bool initHardware(IOService *provider) override;
    void free() override;
    void stop(IOService *provider) override;
    IOReturn performAudioEngineStart() override;
    IOReturn performAudioEngineStop() override;
    UInt32 getCurrentSampleFrame() override;
    // CoreAudio's input read ends in a trap into this method; the base class returns kIOReturnUnsupported, which the HAL turns into a permanent "overload".
    IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream) override;
    IOReturn setProperties(OSObject *props) override;   // runtime tunables (root: tools/engine-tune): tsmode, tick_ms, gain_db
    void setGainFromSlider(SInt32 v);
    DSPDevice *dsp = nullptr;
    SInt32 gain = 75;                       // 0..100, set by the "Input volume" slider
private:
    static void tickAction(OSObject *owner, IOTimerEventSource *sender);
    void tick();
    void tickSynthetic();
    void tickLive();
    void applyGain();
    bool bringUpLive();                     // DSP power-up + pipeline + capture stream + first timestamp (start and recovery)
    void bringDownLive();                   // stop DMA, free stream, power the DSP down
    void recoverLive(const char *why);      // health check reaction: restart, or fall back to silence
    UInt64 framesNow() const;
    IOBufferMemoryDescriptor *ring = nullptr;   // synthetic mode only (live mode uses the DSP's DMA ring)
    void *ringBase = nullptr; UInt32 ringBytes = 0, ringFrames = 0;
    IOAudioStream *stream = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool running = false;
    uint64_t t0 = 0;                        // absolute time of frame 0 of the current run
    UInt32 tsMode = 0, tickMs = 5;          // tunables: 0 = timestamp at the exact wrap time, 1 = at "now" (like an interrupt handler)
    volatile UInt64 gcsfCalls = 0;          // how often CoreAudio asks for the current frame
    volatile UInt64 convCalls = 0, convFrames = 0;
    UInt64 lastStat = 0;
    UInt64 produced = 0, wraps = 0, ticks = 0;   // synthetic: frames generated; both: buffer wraps already stamped
    // live mode
    UInt32 lastLpib = 0, stallTicks = 0; bool errLogged = false;
    volatile float gainLin = 1.0f;          // software gain applied while converting (live mode only)
    SInt32 gainDbOverride = 1000;           // 1000 = follow the slider
    bool rawMode = false;                   // tunable raw=1: left = mic A, right = mic B, no join (diagnostics)
    volatile UInt64 statANz = 0, statBNz = 0; volatile UInt32 statAPeak = 0, statBPeak = 0;   // per-channel stats, logged and reset every 2 s
    bool silent = false, injectStall = false;   // silent: recovery gave up, the engine delivers zeros on a software clock
    int failStarts = 0;                     // fault injection: refuse this many live starts
    UInt32 faultCount = 0; uint64_t lastFault = 0; UInt64 faults = 0, recoveries = 0;
    bool active = false;                    // live: DSP + DMA are up
};
