// Phase 2-4 test harness (boot-arg driven). Since step 5.2 the hardware sequences live in DSPDevice (DSPDevice.cpp);
// this file only decides WHICH stages run and keeps the measurement code: timed capture loop, sample statistics, raw dump to a file.
//
// SAFETY: only reached when boot-arg ohmydsp_p2=1 (live) or =2 (dry run: log only). Boot-args: ohmydsp_p3_n=N (replay N pipeline messages),
// ohmydsp_p3_dump, ohmydsp_p4=1 (read-only precheck) / 2 (stream setup + PCM_PARAMS) / 3 (DMA run 100 ms) / 4 (+TRIG_START 500 ms) /
// 5 (2 s capture to /Library/Application Support/dmic-macos/capture-raw.bin), ohmydsp_p4_sd=N (default 5), ohmydsp_p2_keep.
#include "OhMyDSP.hpp"
#include "DSPDevice.hpp"
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <kern/clock.h>

#define LOG(fmt, ...) do { os_log(OS_LOG_DEFAULT, "OhMyDSP: " fmt, ##__VA_ARGS__); record(fmt, ##__VA_ARGS__); } while (0)

bool OhMyDSP::bootDSP(IOPCIDevice *pci, volatile UInt8 *b0, size_t len0, volatile UInt8 *b4, size_t len4, bool dry)
{
    DSPDevice hw(pci, b0, len0, b4, len4, dry, dspLog, this);
    int p4 = 0, p3n = 0, sdi = 5, dump = 0, keep = 0;
    PE_parse_boot_argn("ohmydsp_p4", &p4, sizeof(p4)); PE_parse_boot_argn("ohmydsp_p3_n", &p3n, sizeof(p3n));
    PE_parse_boot_argn("ohmydsp_p4_sd", &sdi, sizeof(sdi)); PE_parse_boot_argn("ohmydsp_p3_dump", &dump, sizeof(dump));
    PE_parse_boot_argn("ohmydsp_p2_keep", &keep, sizeof(keep));
    OSData *capCopy = nullptr;                          // Phase 4.5: captured samples, written to a file after the DSP is powered down
    bool ok = false;

    // write a root-only file (mode 0600, truncated) from an OSData
    auto saveFile = [&](const char *path, OSData *data) -> bool {
        vfs_context_t ctx = vfs_context_current(); vnode_t vp = nullptr;
        int err = vnode_open(path, O_CREAT | FWRITE | O_TRUNC, 0600, 0, &vp, ctx);
        if (err) { LOG("cannot create %s (err %d)", path, err); return false; }
        int resid = 0;
        err = vn_rdwr(UIO_WRITE, vp, (caddr_t)data->getBytesNoCopy(), (int)data->getLength(), 0, UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        vnode_close(vp, FWRITE, ctx);
        if (err || resid) { LOG("%s: write failed (err %d resid %d)", path, err, resid); return false; }
        LOG("wrote %u bytes to %s", (unsigned)data->getLength(), path); return true; };

    LOG("PHASE2 start: mode=%s (writes are logged before they happen)", dry ? "DRYRUN" : "LIVE");
    if (!hw.checkIdle()) return false;
    if (p4 == 1) return hw.streamPrecheck(sdi);                    // step 4.1: read-only, nothing written
    if (p3n >= 1 && !hw.loadPipelineBlob()) return false;          // software only: validated before any register write
    if (hw.loadFirmware()) {
        ok = hw.powerUp(dump != 0);
        if (ok && p3n >= 1) ok = hw.sendPipeline((UInt32)p3n);

        // Phase 4 steps 4.2-4.5 (need the full 19-message pipeline): capture stream, PCM_PARAMS, DMA RUN, TRIG_START, measure, stop
        if (ok && p4 >= 2 && p4 <= 5) {
            if (dry) LOG("P4.2 skipped: dry run");
            else if (p3n < 19 || hw.ipcMessages() < 19 || sdi < 0 || sdi > 6) LOG("P4.2 skipped: needs ohmydsp_p3_n=19 and a valid input stream (sd=%d)", sdi);
            else {
                bool fail = false;
                const UInt32 capBytes = DSPDevice::CAPTURE_BYTES;
                if (!hw.startStream(p4, (UInt32)sdi)) fail = true;
                else if (p4 >= 3) {
                    const UInt32 window = (p4 == 5) ? 2000 : (p4 == 4) ? 500 : 100;
                    UInt32 lpMax = 0, lp = 0, sts = 0;
                    bool stalled = false, sdErr = false;
                    uint64_t t0 = 0, tn = 0, ns = 0; clock_get_uptime(&t0); UInt64 elapsedUs = 0;
                    const UInt32 marks[] = {1, 10, 50, 100, 200, 300, 500, 1000, 1500, 2000}; UInt32 nextMark = 0;
                    while (elapsedUs < (UInt64)window * 1000) {
                        IOSleep(1);
                        clock_get_uptime(&tn); absolutetime_to_nanoseconds(tn - t0, &ns); elapsedUs = ns / 1000;
                        hw.sample(&lp, &sts); if (lp > lpMax) lpMax = lp;
                        while (nextMark < 10 && elapsedUs >= (UInt64)marks[nextMark] * 1000) { LOG("P4.%d t=%u ms: LPIB=0x%08x (%u bytes) STS=0x%02x", p4, marks[nextMark], lp, lp, sts); nextMark++; }
                        if (sts & 0x18) { LOG("P4.%d SD error flags set (STS=0x%02x): stopping", p4, sts); sdErr = true; break; }
                        if (p4 >= 4 && elapsedUs >= 100000 && lpMax == 0) { LOG("P4.%d LPIB did not move 100 ms after TRIG_START: stopping", p4); stalled = true; break; }
                    }
                    if (p4 >= 4 && elapsedUs) LOG("P4.%d rate check: %u bytes in %llu us = %llu.%llu bytes/ms (expected 384)", p4, lpMax, elapsedUs, (UInt64)lpMax * 1000 / elapsedUs, ((UInt64)lpMax * 10000 / elapsedUs) % 10);
                    DSPDevice::StopInfo si;
                    bool stopOk = hw.stopDma(&si);
                    if (si.lpEnd > lpMax) lpMax = si.lpEnd;
                    LOG("P4.%d after stop: LPIB=0x%08x (max %u bytes seen) STS=0x%02x CTL=0x%08x", p4, si.lpEnd, lpMax, si.stsEnd, si.ctlEnd);
                    const SInt32 *sm = (const SInt32 *)hw.captureBuffer(); const UInt8 *cb = (const UInt8 *)sm;
                    UInt32 changed = 0;
                    for (UInt32 i = 0; i < capBytes; i++) changed += (cb[i] != 0xA5);
                    LOG("P4.%d capture buffer: %u of %u bytes differ from the 0xA5 guard", p4, changed, capBytes);
                    if (p4 >= 4 && lpMax > 0) {   // sample statistics over the region the DMA reported (S32_LE interleaved, 2 ch)
                        UInt32 nb = (lpMax < capBytes ? lpMax : capBytes) & ~7u, n = nb / 4, nz = 0;
                        SInt32 mn = 0x7fffffff, mx = (SInt32)0x80000000; UInt32 peak = 0, guardLeft = 0;
                        for (UInt32 i = 0; i < n; i++) {
                            SInt32 v = sm[i]; if (v < mn) mn = v; if (v > mx) mx = v; if (v) nz++;
                            UInt32 a = v < 0 ? (UInt32)(-(SInt64)v) : (UInt32)v; if (a > peak) peak = a; guardLeft += (v == (SInt32)0xA5A5A5A5);
                        }
                        LOG("P4.4 samples: %u (%u frames) min=%d max=%d peak=%u non-zero=%u guard-pattern words=%u", n, n / 2, mn, mx, peak, nz, guardLeft);
                        if (p4 == 5 && !sdErr && !stalled && stopOk) capCopy = OSData::withBytes(sm, nb);
                        LOG("P4.4 first 8 words: %08x %08x %08x %08x %08x %08x %08x %08x", n > 0 ? (UInt32)sm[0] : 0, n > 1 ? (UInt32)sm[1] : 0, n > 2 ? (UInt32)sm[2] : 0,
                            n > 3 ? (UInt32)sm[3] : 0, n > 4 ? (UInt32)sm[4] : 0, n > 5 ? (UInt32)sm[5] : 0, n > 6 ? (UInt32)sm[6] : 0, n > 7 ? (UInt32)sm[7] : 0);
                    }
                    if (!stopOk || stalled || sdErr) fail = true;
                }
                if (fail) hw.markStreamFailed();
                if (!hw.freeStream()) fail = true;
                LOG("PHASE4.%d end: %s (%s)", p4, fail ? "FAIL" : "PASS", p4 == 5 ? "DMA + TRIG_START ran 2 s, raw capture saved" : p4 == 4 ? "DMA + TRIG_START ran 500 ms" : p4 == 3 ? "DMA ran 100 ms, no TRIG_START" : "DMA never started");
                if (fail) ok = false;
            }
        }
    }
    hw.teardown(ok && keep);
    if (capCopy) { saveFile("/Library/Application Support/dmic-macos/capture-raw.bin", capCopy); capCopy->release(); }
    LOG("PHASE2 end: result=%s", ok ? "FW_READY" : "FAILED/ABORTED");
    return ok;
}
