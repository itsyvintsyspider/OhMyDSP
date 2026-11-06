// Interface between the OhMyDSP kernel extension and userspace (the daemon / test tools). Plain C, shared by both sides.
//
// The kext exposes one IOUserClient (root only, one client at a time) with two memory regions and three external methods:
//   memory type 0   the DMA capture ring: raw S32_LE, 2 channels interleaved (mic A, mic B), ringBytes long, read-only for the client
//   memory type 1   struct OhMyDSPStatus (one page), read-only for the client
//   method 0 start  power the DSP up and start the capture stream   (blocks ~150 ms)
//   method 1 stop   stop the stream and power the DSP down
//   method 2 tune   scalar in: [0] = key (OHMYDSP_TUNE_*), [1] = value
// Position: the ring is written by the HDA DMA engine. `lpib` is the byte position of the DMA at the last kernel tick (5 ms), `laps`
// counts completed laps. Frame number F (counted from the start of the run) lives at ring byte (F * frameBytes) % ringBytes, and the time
// at which ring frame 0 of lap `laps` was written is `lapHostTime` (mach_absolute_time units, same clock in kernel and userspace).
// After a recovery restart `laps` is advanced by one so the sample timeline stays monotonic; frames of the gap are simply missing (epoch changed).
#pragma once
#include <stdint.h>

#define OHMYDSP_SERVICE_CLASS   "OhMyDSP"
#define OHMYDSP_STATUS_MAGIC    0x4F4D4453u     /* 'OMDS' */
#define OHMYDSP_STATUS_VERSION  1u

enum { OHMYDSP_MEM_RING = 0, OHMYDSP_MEM_STATUS = 1 };
enum { OHMYDSP_SEL_START = 0, OHMYDSP_SEL_STOP = 1, OHMYDSP_SEL_TUNE = 2, OHMYDSP_SEL_COUNT = 3 };
enum { OHMYDSP_TUNE_INJECT = 1 };               /* 1 = stall the DMA once, 2 = stall it and refuse the next 3 restarts (fault injection) */
enum { OHMYDSP_STATE_STOPPED = 0, OHMYDSP_STATE_RUNNING = 1, OHMYDSP_STATE_SILENT = 2 };   /* SILENT: recovery gave up, software clock, zeros */

struct OhMyDSPStatus {
    uint32_t magic, version;
    uint32_t ringBytes, frameBytes, rate, channels;
    volatile uint32_t state;
    volatile uint32_t lpib;                     /* ring byte position at the last tick */
    volatile uint32_t seq;                      /* seqlock: odd while laps / lapHostTime are being updated */
    volatile uint32_t epoch;                    /* bumped on every (re)start of the DMA, so a client can detect a discontinuity (recovery) */
    volatile uint64_t laps;                     /* completed laps of the ring since start */
    volatile uint64_t lapHostTime;              /* host time at which ring frame 0 of lap `laps` was written */
    volatile uint64_t faults, recoveries;
};
