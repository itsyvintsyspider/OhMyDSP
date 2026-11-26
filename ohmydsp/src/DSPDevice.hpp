// Phase 5.2: the Phase 2-4 hardware sequences (DSP power-up, firmware download, IPC pipeline replay, host DMA capture stream)
// as one reusable class. Pure refactor of the former DSPBoot.cpp bootDSP(): every register write keeps its order, its mask
// and its log line (regression: diff of the "W ..." lines against logs/phase4-5b-ioreg.txt).
//
// SAFETY (unchanged from Phase 2-4): EVERY register write goes through W*()/U*(), which log BEFORE writing. No interrupts are
// enabled (the MSI belongs to AppleHDAController); all waits poll with bounded timeouts. Never touches GCTL, CORB/RIRB, codec
// verbs, or any stream descriptor other than SD8 (code loader) and the chosen free input stream (default SD5, tag 6).
//
// Call order:  checkIdle -> [loadPipelineBlob] -> loadFirmware -> powerUp -> [sendPipeline] -> startStream ... stopDma ... freeStream -> teardown
// Any call may fail; after a failure the caller still calls freeStream (if startStream ran) and teardown, both idempotent.
#pragma once
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/c++/OSData.h>

class DSPDevice {
public:
    typedef void (*LogFn)(void *ctx, const char *fmt, ...);
    enum : UInt32 { CAPTURE_BYTES = 1u << 20, CAPTURE_PERIOD = 8192 };   // 1 MiB ring, 48 kHz S32 2 ch = 384 B/ms
    // SOF IPC4 pipeline state ABI (ipc4/pipeline.h). Keep these named so a
    // successful mailbox reply cannot hide an accidentally shifted state.
    enum : UInt32 { IPC4_STATE_RESET = 2, IPC4_STATE_PAUSED = 3, IPC4_STATE_RUNNING = 4 };
    struct StopInfo { UInt32 lpEnd = 0, stsEnd = 0, ctlEnd = 0; bool stopped = false, ok = false; };

    static void *operator new(size_t sz);           // kexts have no global operator new
    static void operator delete(void *p, size_t sz);
    DSPDevice(IOPCIDevice *pci, volatile UInt8 *b0, size_t len0, volatile UInt8 *b4, size_t len4, bool dry, LogFn log, void *logCtx);
    ~DSPDevice();                                // frees software resources only, never touches hardware

    // ---- before any register write ----
    bool checkIdle();                            // PRECHECK (decode on, controller up, DSP virgin idle, SD8 untouched) + PP/SPIB capability walk
    bool streamPrecheck(int sdi);                // step 4.1: read-only look at the capture stream + DMA buffer mapping
    bool loadPipelineBlob();                     // validated IPC3/IPC4 replay asset selected by build variant
    bool loadFirmware();                         // sof-cnl.ri from /Library (never committed), window table, DMA buffers + BDL for the code loader
    // ---- hardware ----
    bool powerUp(bool dumpMailbox);              // gating off, SD8 code loader, cores, ROM, firmware DMA, FW_READY
    bool sendPipeline(UInt32 count);             // replay the first `count` blob messages, each acknowledged and checked
    bool sendIpc4CaptureGraph(UInt32 hostSdi);   // replay the IPC4 graph with the host-copier HDA input index patched for this stream
    bool sendIpc4Probe();                         // one reviewed IPC4 pipeline-create round trip, no topology or DMA
    bool sendIpc4PipelineState(UInt32 pipelineId, UInt32 state, const char *what);
    bool sendIpc4PipelineStateMulti(UInt32 state, UInt32 firstPipelineId, UInt32 secondPipelineId, const char *what);
    bool setIpc4Trace(bool enable);               // base-firmware mtrace, capture-DMA diagnostic only
    bool beginIpc4TraceCapture();                 // bounded host archive; consume firmware ring after copying
    void setIpc4TracePhase(UInt32 phase);
    void *ipc4TraceData() const { return traceArchive; }
    UInt32 ipc4TraceSize() const { return traceArchiveBytes; }
    bool startIpc4Session();
    bool stopIpc4Session();
    void serviceIpc4Trace();
    // Capture ring: by default allocated per stream (startStream/freeStream, 1 MiB, guard 0xA5: the Phase 4 harness). The audio engine instead
    // sets a size and allocates ONCE (stable address for IOAudioStream::setSampleBuffer); startStream then only refills it with `fill`.
    bool setCaptureBytes(UInt32 bytes);          // multiple of CAPTURE_PERIOD, 2 periods .. 1 MiB; before allocStreamBuffers
    bool allocStreamBuffers();                   // persistent ring + page table + BDL (software only, no register access)
    void releaseStreamBuffers();                 // only when no stream is active and the DMA is known stopped
    bool startStream(int mode, UInt32 sdi, UInt8 fill = 0xA5);   // mode 2: stream+PCM_PARAMS only, 3: +DMA RUN, 4/5: +TRIG_START
    bool setupIpc4CaptureStream(UInt32 sdi);      // IPC4 host-copier stream setup, without PCM_PARAMS or RUN
    bool startIpc4CaptureDma();                   // arm the HDA stream after IPC4 pipeline RUNNING succeeds
    bool observeIpc4CaptureStream(UInt32 durationMs); // sample an already-running IPC4 HDA stream
    bool captureDescriptorIdle();
    bool finishIpc4CaptureProbe(bool graph, bool trace);
    bool runIpc4CaptureStream(UInt32 durationMs); // short IPC4 DMA run with bounded polling and buffer statistics
    void sample(UInt32 *lpib, UInt32 *sts);      // polled stream position (LPIB) and status byte
    bool stopDma(StopInfo *info);                // TRIG_STOP (if started), clear RUN, wait, read end state, clear STS (idempotent)
    void markStreamFailed() { streamFailed = true; }   // skips PCM_FREE in freeStream (as the Phase 4 failure paths did)
    bool freeStream();                           // stopDma if still running, PCM_FREE, undo the stream registers, free the DMA ring (leaked if RUN is stuck)
    void teardown(bool keepDspPowered);          // SD8 undo, DSP power-down, restore PGCTL/EM2/CGCTL (only what was changed)

    const void *captureBuffer() const { return pb.cap ? pb.cap->getBytesNoCopy() : nullptr; }
    void *captureBase() const { return pb.cap ? pb.cap->getBytesNoCopy() : nullptr; }
    IOMemoryDescriptor *captureMemory() const { return pb.cap; }   // the persistent ring, for mapping into a user task
    UInt32 captureBytes() const { return capBytes; }
    UInt32 ipcMessages() const { return ipcCount; }
    bool dryRun() const { return dry; }

private:
    struct Buf {
        IOBufferMemoryDescriptor *cap = nullptr, *pt = nullptr, *bd = nullptr;
        IODMACommand *capC = nullptr, *ptC = nullptr, *bdC = nullptr;
        UInt32 nent = 0; UInt64 ptIova = 0, bdlIova = 0;
    };
    void W4(UInt32 off, UInt32 v, const char *what);
    void W0(UInt32 off, UInt32 v, const char *what);
    void W0_16(UInt32 off, UInt16 v, const char *what);
    void W0_8(UInt32 off, UInt8 v, const char *what);
    void WC(UInt32 off, UInt32 v, const char *what);
    void U4(UInt32 off, UInt32 mask, UInt32 val, const char *what);
    void U0(UInt32 off, UInt32 mask, UInt32 val, const char *what);
    bool poll(volatile UInt8 *b, UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutMs, const char *what);
    bool ipcTx(const UInt32 *msg, UInt32 len, const char *what, UInt32 minReply, UInt32 *reply);
    bool ipc4Tx(const UInt32 *payload, UInt32 payloadBytes, UInt32 primary, UInt32 extension,
                const char *what, UInt32 *replyPrimary, UInt32 *replyExtension);
    bool ackIpc4Notification(UInt32 primary, const char *what);
    void drainIpc4Trace();
    void romDiag();                              // read-only register snapshot when the ROM does not reach INIT_DONE
    bool watchRomInitDone(UInt32 timeoutMs);
    bool loadFile(const char *path, UInt32 maxSize, OSData **out);
    bool allocBuffers(Buf &b);
    void freeBuffers(Buf &b);

    IOPCIDevice *pci;
    volatile UInt8 *b0, *b4; size_t len0, len4;
    bool dry;
    LogFn logFn; void *logCtx;
    UInt32 ppBase = 0, spibBase = 0;           // BAR0 offsets of PP and SPIB capabilities
    UInt32 sdOff;                              // code-loader stream SD8 register block
    UInt32 savedPG = 0, savedCG = 0, savedEM2 = 0;
    UInt32 downbox = 0, downSize = 0, upbox = 0, upSize = 0;   // mailbox windows (BAR4 offsets) from the firmware's ext manifest
    bool gatingChanged = false, streamTouched = false, dspPowered = false;
    // firmware + pipeline (software only)
    OSData *fwFile = nullptr, *ipcBlob = nullptr;
    UInt32 ipcCount = 0, ipcOff[64] = {0};
    bool ipc4Blob = false;
    IOBufferMemoryDescriptor *fwBuf = nullptr, *bdlBuf = nullptr;
    IODMACommand *fwCmd = nullptr, *bdlCmd = nullptr;
    UInt32 nEnt = 0; UInt64 bdlIova = 0;
    bool fwReady = false;
    enum : UInt32 { TRACE_ARCHIVE_CAPACITY = 65536 };
    UInt8 *traceArchive = nullptr;
    UInt32 traceArchiveBytes = 0, traceSequence = 0, tracePhase = 0;
    bool traceArchiveStopped = false;
    bool sessionTouched = false, sessionGraph = false, sessionTrace = false, sessionBlocked = false;
    // capture stream state
    Buf pb;
    UInt32 sd = 0, sdBit = 0, sdTag = 0; int sdIdx = -1, streamMode = 0;
    UInt32 capBytes = CAPTURE_BYTES, capPages = CAPTURE_BYTES / 4096; bool persistent = false;
    bool streamTouchedHw = false, paramsSent = false, dmaOn = false, triggered = false, streamFailed = false, stuck = false;
    UInt32 ipc4LpibStart = 0;
    UInt64 ipc4DmaStartTime = 0;
};
