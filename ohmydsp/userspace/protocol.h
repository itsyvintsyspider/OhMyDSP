// Daemon <-> AudioServerPlugIn protocol (Phase 6.1c). The driver (inside coreaudiod's sandbox) cannot open the kext's user client, so the root
// daemon opens it and shares a copy of the capture ring with the driver through an XPC shared-memory object.
//   driver -> daemon  {"cmd": "start"} / {"cmd": "stop"}   (sync message, reply carries status and, for start, the shared region)
//   daemon -> driver  {"status": int64, "shm": xpc_shmem, "size": uint64}
// Region layout: struct OhMyDSPShm (4096 bytes) followed by the ring (same geometry as the kernel ring: raw S32_LE, 2 ch, A then B).
#pragma once
#include <stdint.h>

#define OHMYDSP_MACH_SERVICE "com.ohmydsp.daemon"
#define OHMYDSP_SHM_MAGIC    0x4F4D5348u    /* 'OMSH' */
#define OHMYDSP_SHM_HEADER   4096u

struct OhMyDSPShm {
    uint32_t magic, version;
    uint32_t ringBytes, frameBytes, rate, channels;
    volatile uint32_t seq;                  /* seqlock for the fields below: odd while being updated */
    volatile uint32_t epoch;                /* kernel epoch: changes on every DMA restart (discontinuity) */
    volatile uint32_t state;                /* kernel state: 0 stopped, 1 running, 2 silent */
    uint32_t pad;
    volatile uint64_t laps;                 /* kernel lap counter (sample time of ring frame 0 = laps * ringFrames) */
    volatile uint64_t lapHostTime;          /* mach_absolute_time at which ring frame 0 of that lap was written */
    volatile uint64_t written;              /* frames copied into the ring so far, on the same timeline; ring frame index = frame % ringFrames */
};
