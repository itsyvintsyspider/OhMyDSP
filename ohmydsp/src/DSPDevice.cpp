// Phase 5.2: see DSPDevice.hpp. Code moved from DSPBoot.cpp (Phase 2 power-up/firmware, Phase 3 IPC replay, Phase 4 capture stream).
// Mirrors sof-linux/sound/soc/sof/intel cl_dsp_init / hda_cl_* and hda_dsp_stream_* (see docs/phase2-plan.md, docs/phase4-plan.md).
#include "DSPDevice.hpp"
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <kern/clock.h>

#define LOG(fmt, ...) do { os_log(OS_LOG_DEFAULT, "OhMyDSP: " fmt, ##__VA_ARGS__); if (logFn) logFn(logCtx, fmt, ##__VA_ARGS__); } while (0)

// ---- constants (docs/bringup-recipe.md section 2; sof-linux intel/hda.h) ----
enum : UInt32 {
    ADSPCS = 0x04,
    HIPCTDR = 0xC0, HIPCTDA = 0xC4, HIPCTDD = 0xC8,
    HIPCIDR = 0xD0, HIPCIDA = 0xD4, HIPCIDD = 0xD8,
    ROM_STATUS = 0x80000, ROM_ERROR = 0x80004, MBOX_UPLINK = 0x81000,
    IPC4_OUTBOX = 0xA0000, IPC4_DEBUG = 0xC0000, IPC4_MAX_PAYLOAD = 4096,
    IPC4_REPLY_DIR = 1u << 29, IPC4_REPLY_STATUS = 0x00FFFFFF,
    BIT31 = 0x80000000u,
    ROM_IPC_CONTROL = 0x01000000, ROM_IPC_PURGE_FW = 0x00004000, ROM_STREAM_SHIFT = 9,
    FSR_MASK = 0x00FFFFFF, FSR_INIT_DONE = 1, FSR_FW_ENTERED = 5,
    IPC_FW_READY = 0x70000000,
    CORES_ALL = 0xF, CORE0 = 0x1,
    SD_BASE = 0x80, SD_STRIDE = 0x20,
    SD_CTL_SRST = 0x1, SD_CTL_RUN = 0x2, SD_CTL_INTS = 0x1C, SD_TAG_SHIFT = 20,
    SD_STS = 0x03, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12, SD_BDLPL = 0x18, SD_BDLPU = 0x1C,
    HDA_CL_STREAM_FORMAT = 0x40,
    EM2 = 0x1030, EM2_L1SEN = 1u << 13,
    PCI_PGCTL = 0x44, PGCTL_ADSPPGD = 1u << 2, PCI_CGCTL = 0x48, CGCTL_ADSPDCGE = 1u << 1,
#ifdef OHMYDSP_RPL_LIVE
    FW_SIZE_EXPECTED = 707036, PAYLOAD_OFF = 0x9dc,
#else
    FW_SIZE_EXPECTED = 557824, PAYLOAD_OFF = 0x300,
#endif
    STREAM_INDEX = 8, STREAM_TAG = 2,       // free playback descriptor SD8 (Phase 1 log); AppleHDA uses out SD7 tag 1
    P4_PERIOD = DSPDevice::CAPTURE_PERIOD, P4_FMT = 0x0041, P4_COMP_HOST = 50,
};
#ifdef OHMYDSP_RPL_LIVE
static const char *kFwPath = "/Library/Application Support/OhMyDSP/rpl/sof-rpl.ri";
#else
static const char *kFwPath = "/Library/Application Support/OhMyDSP/sof-cnl.ri";
#endif
#if defined(OHMYDSP_RPL_IPC4_MODULE_PROBE) || defined(OHMYDSP_RPL_IPC4_BIND_PROBE) || defined(OHMYDSP_RPL_IPC4_CAPTURE_PROBE) || defined(OHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE) || defined(OHMYDSP_RPL_IPC4_SESSION)
static const char *kBlobPath = "/Library/Application Support/OhMyDSP/rpl/rpl-dmic.i4pc";
#else
static const char *kBlobPath = "/Library/Application Support/OhMyDSP/dmic-p10.ipc";
#endif

static inline UInt32 r32(volatile UInt8 *b, UInt32 o) { return *(volatile UInt32 *)(b + o); }
static inline UInt16 r16(volatile UInt8 *b, UInt32 o) { return *(volatile UInt16 *)(b + o); }
static inline void raw32(volatile UInt8 *b, UInt32 o, UInt32 v) { *(volatile UInt32 *)(b + o) = v; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void raw16(volatile UInt8 *b, UInt32 o, UInt16 v) { *(volatile UInt16 *)(b + o) = v; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void raw8(volatile UInt8 *b, UInt32 o, UInt8 v) { *(volatile UInt8 *)(b + o) = v; __atomic_thread_fence(__ATOMIC_SEQ_CST); }

DSPDevice::DSPDevice(IOPCIDevice *p, volatile UInt8 *bar0, size_t l0, volatile UInt8 *bar4, size_t l4, bool dryRun, LogFn log, void *ctx)
    : pci(p), b0(bar0), b4(bar4), len0(l0), len4(l4), dry(dryRun), logFn(log), logCtx(ctx), sdOff(SD_BASE + SD_STRIDE * STREAM_INDEX) {}

void *DSPDevice::operator new(size_t sz) { return IOMalloc(sz); }
void DSPDevice::operator delete(void *p, size_t sz) { if (p) IOFree(p, sz); }

bool DSPDevice::setCaptureBytes(UInt32 bytes)
{
    if (pb.cap || bytes % P4_PERIOD || bytes < 2 * P4_PERIOD || bytes > CAPTURE_BYTES) return false;
    capBytes = bytes; capPages = bytes / 4096; return true;
}
bool DSPDevice::allocStreamBuffers()
{
    if (pb.cap) return true;
    if (!allocBuffers(pb)) { freeBuffers(pb); return false; }
    persistent = true; return true;
}
void DSPDevice::releaseStreamBuffers()
{
    if (sdIdx >= 0 || dmaOn || stuck) return;   // never pull the ring from under a running DMA engine
    freeBuffers(pb); persistent = false;
}

DSPDevice::~DSPDevice()
{
    if (traceArchive) IOFree(traceArchive, TRACE_ARCHIVE_CAPACITY);
    if (!stuck) freeBuffers(pb);   // a stream whose RUN bit could not be cleared keeps its buffers (deliberate leak, see freeStream)
    if (fwCmd) { fwCmd->clearMemoryDescriptor(); fwCmd->release(); }
    if (bdlCmd) { bdlCmd->clearMemoryDescriptor(); bdlCmd->release(); }
    if (fwBuf) fwBuf->release();
    if (bdlBuf) bdlBuf->release();
    if (fwFile) fwFile->release();
    if (ipcBlob) ipcBlob->release();
}

// ---- write helpers (log, then write unless dry-run) ----
void DSPDevice::W4(UInt32 off, UInt32 v, const char *what) {
    LOG("%sW BAR4+0x%05x <- 0x%08x  (%s)", dry ? "DRYRUN " : "", off, v, what); if (!dry) raw32(b4, off, v); }
void DSPDevice::W0(UInt32 off, UInt32 v, const char *what) {
    LOG("%sW BAR0+0x%05x <- 0x%08x  (%s)", dry ? "DRYRUN " : "", off, v, what); if (!dry) raw32(b0, off, v); }
void DSPDevice::W0_16(UInt32 off, UInt16 v, const char *what) {
    LOG("%sW16 BAR0+0x%05x <- 0x%04x  (%s)", dry ? "DRYRUN " : "", off, v, what); if (!dry) raw16(b0, off, v); }
void DSPDevice::W0_8(UInt32 off, UInt8 v, const char *what) {
    LOG("%sW8 BAR0+0x%05x <- 0x%02x  (%s)", dry ? "DRYRUN " : "", off, v, what); if (!dry) raw8(b0, off, v); }
void DSPDevice::WC(UInt32 off, UInt32 v, const char *what) {
    LOG("%sW CFG+0x%02x <- 0x%08x  (%s)", dry ? "DRYRUN " : "", off, v, what); if (!dry) pci->configWrite32(off, v); }
// read-modify-write of one 32-bit register (never whole-register overwrite of shared regs)
void DSPDevice::U4(UInt32 off, UInt32 mask, UInt32 val, const char *what) { W4(off, (r32(b4, off) & ~mask) | val, what); }
void DSPDevice::U0(UInt32 off, UInt32 mask, UInt32 val, const char *what) { W0(off, (r32(b0, off) & ~mask) | val, what); }
// poll: true when (reg & mask) == want within timeoutMs. In dry-run: logs and reports success.
bool DSPDevice::poll(volatile UInt8 *b, UInt32 off, UInt32 mask, UInt32 want, UInt32 timeoutMs, const char *what)
{
    if (dry) { LOG("DRYRUN poll %s (mask 0x%x want 0x%x, %u ms)", what, mask, want, timeoutMs); return true; }
    UInt32 v = 0;
    for (UInt32 us = 0; us <= timeoutMs * 1000; us += (timeoutMs > 50 ? 1000 : 100)) {
        v = r32(b, off);
        if ((v & mask) == want) { LOG("poll %s ok (0x%08x)", what, v); return true; }
        if (timeoutMs > 50) IOSleep(1); else IODelay(100);
    }
    LOG("poll %s TIMEOUT (last 0x%08x, mask 0x%x want 0x%x)", what, v, mask, want);
    return false;
}

// read a small root-owned file into an OSData (no hardware access)
bool DSPDevice::loadFile(const char *path, UInt32 maxSize, OSData **out)
{
    vfs_context_t ctx = vfs_context_current(); vnode_t vp = nullptr;
    int err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    if (err) { LOG("cannot open %s (err %d)", path, err); return false; }
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    off_t sz = (vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_data_size)) ? (off_t)va.va_data_size : -1;
    if (sz <= 0 || sz > (off_t)maxSize) { LOG("%s: bad size %lld", path, (long long)sz); vnode_close(vp, FREAD, ctx); return false; }
    UInt8 *tmp = (UInt8 *)IOMalloc((size_t)sz); int resid = 0;
    err = tmp ? vn_rdwr(UIO_READ, vp, (caddr_t)tmp, (int)sz, 0, UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) : ENOMEM;
    vnode_close(vp, FREAD, ctx);
    if (err || resid) { LOG("%s: read failed (err %d resid %d)", path, err, resid); if (tmp) IOFree(tmp, (size_t)sz); return false; }
    *out = OSData::withBytes(tmp, (unsigned)sz); IOFree(tmp, (size_t)sz);
    return *out != nullptr;
}

// ---- preconditions (no writes) ----
bool DSPDevice::checkIdle()
{
    UInt16 cmd = pci->configRead16(0x04);
    UInt32 adspcs = r32(b4, ADSPCS);
    UInt32 gctl = r32(b0, 0x08);
    UInt32 sdCtl = r32(b0, sdOff), sdCbl = r32(b0, sdOff + SD_CBL);
    LOG("PRECHECK cmd=0x%04x ADSPCS=0x%08x GCTL=0x%08x SD%u CTL=0x%08x CBL=0x%08x", cmd, adspcs, gctl, STREAM_INDEX, sdCtl, sdCbl);
    if (!(cmd & 2) || (gctl & 1) == 0) { LOG("ABORT: memory decode off or HDA controller not out of reset"); return false; }
    if (adspcs != 0x00000f0f) { LOG("ABORT: DSP not in the virgin idle state (ADSPCS != 0x00000f0f)"); return false; }
    if ((sdCtl & ~0x00040000u) != 0) { LOG("ABORT: SD%u CTL not in the untouched state", STREAM_INDEX); return false; }
    if (sdCbl != 0) { LOG("ABORT: SD%u CBL non-zero, in use", STREAM_INDEX); return false; }
    // capability walk (read-only) for PP (id 3) and SPIB (id 4)
    {
        UInt32 off = r32(b0, 0x14) & 0xffff;
        for (int i = 0; i < 16 && off && !(off & 3) && off + 8 <= len0; i++) {
            UInt32 cap = r32(b0, off); if (cap == 0xffffffff) break;
            UInt32 id = (cap >> 16) & 0xfff;
            if (id == 3) ppBase = off; else if (id == 4) spibBase = off;
            off = cap & 0xffff;
        }
    }
    if (!ppBase || !spibBase) { LOG("ABORT: PP/SPIB capability not found (pp=0x%x spib=0x%x)", ppBase, spibBase); return false; }
    LOG("PP cap @0x%x, SPIB cap @0x%x", ppBase, spibBase);
    return true;
}

// ---- Phase 4 buffers (software only: allocation, IOMMU mappings, BDL + page table contents, guard pattern) ----
void DSPDevice::freeBuffers(Buf &b)
{
    if (b.capC) { b.capC->clearMemoryDescriptor(); b.capC->release(); }
    if (b.ptC) { b.ptC->clearMemoryDescriptor(); b.ptC->release(); }
    if (b.bdC) { b.bdC->clearMemoryDescriptor(); b.bdC->release(); }
    if (b.cap) b.cap->release();
    if (b.pt) b.pt->release();
    if (b.bd) b.bd->release();
    b = Buf();
}

bool DSPDevice::allocBuffers(Buf &b)
{
    b.cap = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, capBytes, 0x00000000FFFFF000ULL);
    b.pt = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, 4096, 0x00000000FFFFF000ULL);
    b.bd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, 4096, 0x0000FFFFFFFFF000ULL);
    b.capC = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, 0, IODMACommand::kMapped, 0, 1);
    b.ptC = IODMACommand::withSpecification(kIODMACommandOutputHost64, 32, 0, IODMACommand::kMapped, 0, 1);
    b.bdC = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
    if (!b.cap || !b.pt || !b.bd || !b.capC || !b.ptC || !b.bdC || b.capC->setMemoryDescriptor(b.cap) != kIOReturnSuccess ||
        b.ptC->setMemoryDescriptor(b.pt) != kIOReturnSuccess || b.bdC->setMemoryDescriptor(b.bd) != kIOReturnSuccess) {
        LOG("P4 buffers: allocation / IODMACommand mapping failed"); return false; }
    memset(b.cap->getBytesNoCopy(), 0xA5, capBytes);   // guard pattern
    UInt32 bad = 0; const UInt8 *cb = (const UInt8 *)b.cap->getBytesNoCopy();
    for (UInt32 i = 0; i < capBytes; i++) bad += (cb[i] != 0xA5);
    LOG("P4 capture buffer %u bytes, guard 0xA5 written, %u mismatches", capBytes, bad);
    if (bad) return false;
    UInt64 *pg = (UInt64 *)IOMalloc(capPages * sizeof(UInt64)); if (!pg) return false;
    UInt32 npg = 0, nseg = 0, nent = 0; bool segOk = true, below4g = true;
    UInt32 *bdl = (UInt32 *)b.bd->getBytesNoCopy(); memset(bdl, 0, 4096);
    UInt64 off = 0;
    while (segOk && off < capBytes) {
        IODMACommand::Segment64 seg; UInt32 n = 1; UInt64 start = off;
        if (b.capC->gen64IOVMSegments(&off, &seg, &n) != kIOReturnSuccess || n != 1 || off == start) { segOk = false; break; }
        if ((seg.fIOVMAddr | seg.fLength) & 0xFFF) { LOG("P4 segment not 4K aligned (0x%llx len 0x%llx)", seg.fIOVMAddr, (UInt64)seg.fLength); segOk = false; break; }
        if (nseg < 4) LOG("P4 cap segment %u: IOVA 0x%llx len 0x%llx at buffer offset 0x%llx", nseg, seg.fIOVMAddr, (UInt64)seg.fLength, start);
        nseg++;
        if (seg.fIOVMAddr + seg.fLength > 0x100000000ULL) below4g = false;
        for (UInt64 p = 0; p < seg.fLength && npg < capPages; p += 4096) pg[npg++] = seg.fIOVMAddr + p;
        UInt64 cur = start, left = seg.fLength, addr = seg.fIOVMAddr;   // BDL entries split on PERIOD boundaries of the buffer offset
        while (left) {
            UInt64 take = P4_PERIOD - (cur % P4_PERIOD); if (take > left) take = left;
            if (nent >= 256) { segOk = false; break; }
            bdl[nent * 4 + 0] = (UInt32)addr; bdl[nent * 4 + 1] = (UInt32)(addr >> 32); bdl[nent * 4 + 2] = (UInt32)take; bdl[nent * 4 + 3] = 0;
            nent++; cur += take; addr += take; left -= take;
        }
    }
    LOG("P4 capture buffer: %u segments, %u pages, %u BDL entries (period %u B), all IOVA < 4 GB: %s", nseg, npg, nent, (UInt32)P4_PERIOD, below4g ? "YES" : "NO");
    bool good = segOk && npg == capPages && below4g && nent >= 2;
    if (good) {
        // page table (snd_sof_create_page_table): 20-bit PFNs, 5 bytes per 2 pages, little endian; decode and compare
        UInt8 *pt = (UInt8 *)b.pt->getBytesNoCopy(); memset(pt, 0, 4096);
        for (UInt32 i = 0; i < capPages; i++) {
            UInt32 idx = (5 * i) >> 1, pfn = (UInt32)(pg[i] >> 12), v = (i & 1) ? ((pt[idx] & 0xF) | (pfn << 4)) : pfn;
            pt[idx] = (UInt8)v; pt[idx + 1] = (UInt8)(v >> 8); pt[idx + 2] = (UInt8)(v >> 16); pt[idx + 3] = (UInt8)(v >> 24);
        }
        UInt32 mism = 0;
        for (UInt32 i = 0; i < capPages; i++) {
            UInt32 idx = (5 * i) >> 1, v = (UInt32)pt[idx] | ((UInt32)pt[idx + 1] << 8) | ((UInt32)pt[idx + 2] << 16) | ((UInt32)pt[idx + 3] << 24);
            mism += (((i & 1) ? ((v >> 4) & 0xFFFFF) : (v & 0xFFFFF)) != (UInt32)(pg[i] >> 12));
        }
        UInt64 po = 0; IODMACommand::Segment64 ps; UInt32 pn = 1;
        bool ptOk = b.ptC->gen64IOVMSegments(&po, &ps, &pn) == kIOReturnSuccess && ps.fLength >= 4096 && ps.fIOVMAddr < 0x100000000ULL;
        UInt64 bo = 0; IODMACommand::Segment64 bs; UInt32 bn = 1;
        bool bdOk = b.bdC->gen64IOVMSegments(&bo, &bs, &bn) == kIOReturnSuccess && bs.fLength >= 4096;
        if (ptOk) b.ptIova = ps.fIOVMAddr; if (bdOk) b.bdlIova = bs.fIOVMAddr; b.nent = nent;
        LOG("P4 page table %u bytes, decode mismatches %u; page-table IOVA 0x%llx (<4 GB: %s); BDL IOVA 0x%llx (single page: %s)",
            (capPages * 5 + 1) / 2, mism, b.ptIova, ptOk ? "YES" : "NO", b.bdlIova, bdOk ? "YES" : "NO");
        LOG("P4 BDL entry0: addr=0x%08x%08x len=%u; entry%u: addr=0x%08x%08x len=%u", bdl[1], bdl[0], bdl[2], nent - 1, bdl[(nent - 1) * 4 + 1], bdl[(nent - 1) * 4], bdl[(nent - 1) * 4 + 2]);
        good = !mism && ptOk && bdOk;
    } else LOG("P4 buffer layout unusable for BDL/page table");
    IOFree(pg, capPages * sizeof(UInt64));
    return good;
}

// ---- Phase 4 step 4.1: READ-ONLY precheck of the capture stream + DMA buffer mapping. No hardware register is written and the DSP
// is not touched. The only side effect is creating (and freeing) IOMMU mappings. ----
bool DSPDevice::streamPrecheck(int sdi)
{
    LOG("PHASE4.1 start (read-only): candidate capture stream SD%d tag %d", sdi, sdi + 1);
    LOG("P4.1 GCAP=0x%04x INTCTL=0x%08x INTSTS=0x%08x DPLBASE=0x%08x DPUBASE=0x%08x PPCTL=0x%08x SPBFCCTL=0x%08x", r16(b0, 0), r32(b0, 0x20), r32(b0, 0x24),
        r32(b0, 0x70), r32(b0, 0x74), r32(b0, ppBase + 4), r32(b0, spibBase + 4));
    for (int i = 0; i < 16; i++) {
        UInt32 s = SD_BASE + SD_STRIDE * i;
        LOG("P4.1 SD%d: CTL/STS=0x%08x LPIB=0x%08x CBL=0x%08x LVI=0x%04x FIFOS=0x%04x FMT=0x%04x BDL=0x%08x%08x SPIB=0x%08x", i, r32(b0, s), r32(b0, s + 4),
            r32(b0, s + SD_CBL), r16(b0, s + SD_LVI), r16(b0, s + 0x10), r16(b0, s + SD_FMT), r32(b0, s + SD_BDLPU), r32(b0, s + SD_BDLPL),
            r32(b0, spibBase + 8 + 8 * i));
    }
    bool pass = true;
    if (sdi < 0 || sdi > 6) { LOG("P4.1 FAIL: SD%d is not an input stream (GCAP: 7 inputs)", sdi); pass = false; }
    else {
        UInt32 s = SD_BASE + SD_STRIDE * sdi, ctl = r32(b0, s);
        bool idle = !(ctl & SD_CTL_RUN) && ((ctl >> SD_TAG_SHIFT) & 0xF) == 0 && r32(b0, s + SD_CBL) == 0 &&
                    !(r32(b0, ppBase + 4) & (1u << sdi)) && !(r32(b0, spibBase + 4) & (1u << sdi));
        LOG("P4.1 SD%d idle/unused (RUN=0, tag=0, CBL=0, PPCTL bit=0, SPIB off): %s", sdi, idle ? "YES" : "NO");
        if (!idle) pass = false;
    }
    Buf t; if (!allocBuffers(t)) pass = false;
    freeBuffers(t);
    LOG("PHASE4.1 end: %s (no register written, DSP not touched)", pass ? "PASS" : "FAIL");
    return pass;
}

// ---- Phase 3 message blob (no hardware): load + validate before any register write ----
bool DSPDevice::loadPipelineBlob()
{
    if (!loadFile(kBlobPath, 16384, &ipcBlob)) { LOG("ABORT: Phase 3 blob not available"); return false; }
    const UInt8 *bp = (const UInt8 *)ipcBlob->getBytesNoCopy(); UInt32 bl = ipcBlob->getLength(), pos = 8;
    bool isIpc4 = bl >= 8 && memcmp(bp, "I4PC", 4) == 0;
#ifdef OHMYDSP_RPL_LIVE
    bool good = isIpc4;
#else
    bool good = bl >= 8 && memcmp(bp, "DIPC", 4) == 0;
#endif
    UInt32 cnt = good ? *(const UInt32 *)(bp + 4) : 0;
    if (cnt == 0 || cnt > 64) good = false;
    for (UInt32 i = 0; good && i < cnt; i++) {
        if (pos + 8 > bl) { good = false; break; }
        UInt32 len = *(const UInt32 *)(bp + pos);
        if (isIpc4) {
            // I4PC record: {u32 byte_len, u32 extension, u32 primary, payload...}.
            if (len < 12 || len > IPC4_MAX_PAYLOAD || pos + ((len + 3) & ~3u) > bl) { good = false; break; }
            ipcOff[i] = pos;
            pos += (len + 3) & ~3u;
        } else {
            if (len < 12 || len > 384 || pos + 8 + ((len + 3) & ~3u) > bl || *(const UInt32 *)(bp + pos + 8) != len) { good = false; break; }  // hdr.size == len
            ipcOff[i] = pos; pos += 8 + ((len + 3) & ~3u);
        }
    }
    if (good && pos != bl) good = false;
    LOG("Phase %s blob: %u messages, %u bytes -> %s", isIpc4 ? "IPC4" : "3", cnt, bl, good ? "OK" : "BAD");
    if (!good) { ipcBlob->release(); ipcBlob = nullptr; return false; }
    ipcCount = cnt; ipc4Blob = isIpc4;
    return true;
}

// ---- firmware file + code-loader DMA buffers (no register writes) ----
bool DSPDevice::loadFirmware()
{
    const UInt32 payload = FW_SIZE_EXPECTED - PAYLOAD_OFF;
    {
        vfs_context_t ctx = vfs_context_current();
        vnode_t vp = nullptr;
        int err = vnode_open(kFwPath, FREAD, 0, 0, &vp, ctx);
        if (err) { LOG("ABORT: cannot open firmware %s (err %d). See docs/phase2-plan.md", kFwPath, err); return false; }
        struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
        off_t sz = (vnode_getattr(vp, &va, ctx) == 0 && VATTR_IS_SUPPORTED(&va, va_data_size)) ? (off_t)va.va_data_size : -1;
        if (sz != FW_SIZE_EXPECTED) { LOG("ABORT: firmware size %lld != %d", (long long)sz, FW_SIZE_EXPECTED); vnode_close(vp, FREAD, ctx); return false; }
        UInt8 *tmp = (UInt8 *)IOMalloc(FW_SIZE_EXPECTED);
        int resid = 0;
        err = tmp ? vn_rdwr(UIO_READ, vp, (caddr_t)tmp, FW_SIZE_EXPECTED, 0, UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) : ENOMEM;
        vnode_close(vp, FREAD, ctx);
        if (err || resid) { LOG("ABORT: firmware read failed (err %d resid %d)", err, resid); if (tmp) IOFree(tmp, FW_SIZE_EXPECTED); return false; }
        UInt32 magic = *(UInt32 *)tmp, full = *(UInt32 *)(tmp + 4);
#ifdef OHMYDSP_RPL_LIVE
        bool good = memcmp(tmp, "$AE1", 4) == 0 && full == PAYLOAD_OFF && memcmp(tmp + PAYLOAD_OFF, "$CPD", 4) == 0;
#else
        bool good = magic == 0x6e614d58 && full == PAYLOAD_OFF && memcmp(tmp + PAYLOAD_OFF, "$CPD", 4) == 0;
#endif
        LOG("firmware: magic=0x%08x ext_man_size=0x%x payload[0..3]=%02x%02x%02x%02x -> %s", magic, full,
            tmp[PAYLOAD_OFF], tmp[PAYLOAD_OFF + 1], tmp[PAYLOAD_OFF + 2], tmp[PAYLOAD_OFF + 3], good ? "OK" : "BAD");
        if (!good) { IOFree(tmp, FW_SIZE_EXPECTED); return false; }
 #ifndef OHMYDSP_RPL_LIVE
        {   // window table from the extended manifest (SOF_EXT_MAN_ELEM_WINDOW=1), as Linux does in ipc3_get_windows (ipc3.c:647)
            // layout: ext_man header {magic, full_size, header_size, version}; elems {type, size, sof_ipc_window{ext_hdr(12), num, elem[24 B]}}
            // window elem: hdr(4) type id flags size offset (24 B); BAR4 offset = 0x80000 + id * 0x20000 + offset (SRAM_WINDOW_OFFSET)
            UInt32 pos = *(UInt32 *)(tmp + 8);
            while (pos + 8 <= full) {
                UInt32 et = *(UInt32 *)(tmp + pos), es = *(UInt32 *)(tmp + pos + 4);
                if (es < 8 || pos + es > full) break;
                if (et == 1 && es >= 8 + 16) {
                    UInt32 nw = *(UInt32 *)(tmp + pos + 8 + 12);
                    for (UInt32 i = 0; i < nw && 8 + 16 + 24 * (i + 1) <= es; i++) {
                        const UInt32 *e = (const UInt32 *)(tmp + pos + 8 + 16 + 24 * i);
                        UInt32 wtype = e[1], wid = e[2], wsize = e[4], woff = e[5];
                        UInt32 bar4 = 0x80000 + wid * 0x20000 + woff;
                        LOG("manifest window[%u]: type=%u id=%u size=0x%x off=0x%x -> BAR4 0x%05x", i, wtype, wid, wsize, woff, bar4);
                        if (wtype == 0) { downbox = bar4; downSize = wsize; }   // SOF_IPC_REGION_DOWNBOX (host -> DSP, also carries replies)
                        if (wtype == 1) { upbox = bar4; upSize = wsize; }       // SOF_IPC_REGION_UPBOX (DSP -> host)
                    }
                }
                pos += es;
            }
        }
 #else
        // IPC4 on cAVS 2.5 uses fixed windows.  The IPC3 FW_READY extension
        // fields are zero on this image and must not be used as offsets.
        if (len4 < IPC4_OUTBOX + IPC4_MAX_PAYLOAD || len4 < MBOX_UPLINK + IPC4_MAX_PAYLOAD) {
            LOG("ABORT: BAR4 too small for fixed IPC4 windows (len=0x%zx)", len4);
            IOFree(tmp, FW_SIZE_EXPECTED);
            return false;
        }
        downbox = IPC4_OUTBOX;
        downSize = 0x20000;       // complete window 1, bounded sends use IPC4_MAX_PAYLOAD
        upbox = MBOX_UPLINK;
        upSize = IPC4_MAX_PAYLOAD;
        LOG("RPL IPC4 fixed windows: uplink=0x%05x size=0x%x outbox=0x%05x window=0x%x payload-limit=0x%x",
            upbox, upSize, downbox, downSize, IPC4_MAX_PAYLOAD);
 #endif
        fwFile = OSData::withBytes(tmp + PAYLOAD_OFF, payload);   // only the stripped payload is DMA'd
        IOFree(tmp, FW_SIZE_EXPECTED);
        if (!fwFile) { LOG("ABORT: out of memory"); return false; }
    }

    // DMA buffers (no register writes yet)
    const UInt64 mask = 0x0000FFFFFFFFF000ULL;
    fwBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, payload, mask);
    bdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut, 4096, mask);
    if (!fwBuf || !bdlBuf) { LOG("ABORT: DMA buffer alloc failed"); return false; }
    memcpy(fwBuf->getBytesNoCopy(), fwFile->getBytesNoCopy(), payload);
    memset(bdlBuf->getBytesNoCopy(), 0, 4096);
    fwCmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
    bdlCmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, 64, 0, IODMACommand::kMapped, 0, 1);
    if (!fwCmd || !bdlCmd || fwCmd->setMemoryDescriptor(fwBuf) != kIOReturnSuccess || bdlCmd->setMemoryDescriptor(bdlBuf) != kIOReturnSuccess) {
        LOG("ABORT: IODMACommand setup failed"); return false; }
    {   // BDL entries {addr_lo, addr_hi, len, ioc}; >= 2 entries required (LVI >= 1)
        UInt32 *bdl = (UInt32 *)bdlBuf->getBytesNoCopy();
        UInt64 offset = 0;
        while (offset < payload) {
            IODMACommand::Segment64 seg; UInt32 n = 1; UInt64 prev = offset;
            if (fwCmd->gen64IOVMSegments(&offset, &seg, &n) != kIOReturnSuccess || n != 1 || offset == prev) { LOG("ABORT: BDL segment gen failed"); return false; }
            if (nEnt >= 255) { LOG("ABORT: too many BDL segments"); return false; }
            bdl[nEnt * 4 + 0] = (UInt32)seg.fIOVMAddr; bdl[nEnt * 4 + 1] = (UInt32)(seg.fIOVMAddr >> 32);
            bdl[nEnt * 4 + 2] = (UInt32)seg.fLength;   bdl[nEnt * 4 + 3] = 0;
            nEnt++;
        }
        if (nEnt == 1) {   // contiguous: split in two halves (both 4K aligned) like hda_dsp_stream_setup_bdl()
            UInt64 a = ((UInt64)bdl[1] << 32) | bdl[0]; UInt32 half = payload / 2, rest = payload - half;
            bdl[2] = half;
            bdl[4] = (UInt32)(a + half); bdl[5] = (UInt32)((a + half) >> 32); bdl[6] = rest; bdl[7] = 0;
            nEnt = 2;
        }
        UInt64 o2 = 0; IODMACommand::Segment64 s2; UInt32 n2 = 1;
        if (bdlCmd->gen64IOVMSegments(&o2, &s2, &n2) != kIOReturnSuccess || s2.fLength < 4096) { LOG("ABORT: BDL page not single segment"); return false; }
        bdlIova = s2.fIOVMAddr;
        LOG("DMA: fw %u bytes in %u BDL entries, BDL IOVA 0x%llx (entry0 addr=0x%08x%08x len=0x%x)", payload, nEnt, bdlIova, bdl[1], bdl[0], bdl[2]);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return true;
}

// ================= hardware writes start here =================
// Returns true when the firmware reported FW_READY. On false the caller still calls teardown().
// Read-only snapshot of everything that could explain a ROM that does not reach INIT_DONE.
void DSPDevice::romDiag()
{
    UInt32 cmd = pci->configRead16(0x04), cg = pci->configRead32(PCI_CGCTL), pg = pci->configRead32(PCI_PGCTL);
    LOG("ROMDIAG: ROM_STATUS=0x%08x ROM_ERROR=0x%08x ADSPCS=0x%08x ADSPIC=0x%08x ADSPIS=0x%08x", r32(b4, ROM_STATUS), r32(b4, ROM_ERROR), r32(b4, ADSPCS), r32(b4, 0x08), r32(b4, 0x0C));
    LOG("ROMDIAG: HIPCIDR=0x%08x HIPCIDA=0x%08x HIPCTDR=0x%08x HIPCTDA=0x%08x HIPCCTL=0x%08x", r32(b4, HIPCIDR), r32(b4, HIPCIDA), r32(b4, HIPCTDR), r32(b4, HIPCTDA), r32(b4, 0xE8));
    LOG("ROMDIAG: PCI cmd=0x%04x CGCTL=0x%08x PGCTL=0x%08x EM2=0x%08x GCTL=0x%08x INTCTL=0x%08x STATESTS=0x%04x PPCTL=0x%08x",
        cmd, cg, pg, r32(b0, EM2), r32(b0, 0x08), r32(b0, 0x20), (unsigned)(*(volatile UInt16 *)(b0 + 0x0E)), ppBase ? r32(b0, ppBase + 0x04) : 0);
    LOG("ROMDIAG: SD8 CTL=0x%08x LPIB=0x%08x CBL=0x%08x LVI=0x%08x", r32(b0, sdOff), r32(b0, sdOff + 0x04), r32(b0, sdOff + 0x08), r32(b0, sdOff + 0x0C));
}

// Polls ROM_STATUS every 20 ms for up to timeoutMs, logging each change; true as soon as INIT_DONE shows up.
bool DSPDevice::watchRomInitDone(UInt32 timeoutMs)
{
    UInt32 last = r32(b4, ROM_STATUS);
    for (UInt32 t = 0; t < timeoutMs; t += 20) {
        IOSleep(20);
        UInt32 st = r32(b4, ROM_STATUS);
        if (st != last) { LOG("ROMDIAG: +%u ms ROM_STATUS 0x%08x -> 0x%08x (ROM_ERROR=0x%08x)", (unsigned)t + 20, last, st, r32(b4, ROM_ERROR)); last = st; }
        if ((st & FSR_MASK) == FSR_INIT_DONE) { LOG("ROMDIAG: INIT_DONE reached late, +%u ms", (unsigned)t + 20); return true; }
    }
    LOG("ROMDIAG: no INIT_DONE after %u ms more: ROM_STATUS=0x%08x ROM_ERROR=0x%08x", (unsigned)timeoutMs, r32(b4, ROM_STATUS), r32(b4, ROM_ERROR));
    return false;
}

bool DSPDevice::powerUp(bool dumpMailbox)
{
    const UInt32 payload = FW_SIZE_EXPECTED - PAYLOAD_OFF;
    bool ok = false;
    // step 1/2: save + disable gating (hda_dsp_ctrl_clock_power_gating(false))
    savedCG = pci->configRead32(PCI_CGCTL); savedPG = pci->configRead32(PCI_PGCTL); savedEM2 = r32(b0, EM2);
    LOG("saved CGCTL=0x%08x PGCTL=0x%08x EM2=0x%08x", savedCG, savedPG, savedEM2);
    gatingChanged = true;
    WC(PCI_CGCTL, savedCG & ~CGCTL_ADSPDCGE, "CGCTL: clear ADSPDCGE");
    W0(EM2, savedEM2 & ~EM2_L1SEN, "EM2: clear L1SEN");
    WC(PCI_PGCTL, savedPG | PGCTL_ADSPPGD, "PGCTL: set ADSPPGD");

    // step 3-6: prepare code-loader stream SD8 (hda_dsp_stream_hw_params + spib)
    streamTouched = true;
    {
        const UInt32 s = sdOff, bit = 1u << STREAM_INDEX;
        U0(ppBase + 4, bit, bit, "PPCTL: decouple SD8");
        U0(s, SD_CTL_RUN | SD_CTL_INTS, 0, "SD8 CTL: clear RUN/int enables");
        U0(s, SD_CTL_SRST, SD_CTL_SRST, "SD8 CTL: enter stream reset");
        if (!poll(b0, s, SD_CTL_SRST, SD_CTL_SRST, 5, "SD8 in reset")) return false;
        U0(s, SD_CTL_SRST, 0, "SD8 CTL: leave stream reset");
        if (!poll(b0, s, SD_CTL_SRST, 0, 5, "SD8 out of reset")) return false;
        W0(s + SD_BDLPL, 0, "SD8 BDLPL clear"); W0(s + SD_BDLPU, 0, "SD8 BDLPU clear");
        W0_8(s + SD_STS, SD_CTL_INTS, "SD8 STS clear (w1c)");
        U0(s, 0xF << SD_TAG_SHIFT, (UInt32)STREAM_TAG << SD_TAG_SHIFT, "SD8 CTL: stream tag");
        W0(s + SD_CBL, payload, "SD8 CBL");
        W0_16(s + SD_FMT, HDA_CL_STREAM_FORMAT, "SD8 FORMAT");
        W0_16(s + SD_LVI, (UInt16)(nEnt - 1), "SD8 LVI");
        W0(s + SD_BDLPL, (UInt32)bdlIova, "SD8 BDLPL"); W0(s + SD_BDLPU, (UInt32)(bdlIova >> 32), "SD8 BDLPU");
        U0(spibBase + 4, bit, bit, "SPBFCCTL: enable SPIB for SD8");
        W0(spibBase + 0x08 + 0x08 * STREAM_INDEX, payload, "SPIB[8] = fw size");
    }

    // step 7: power up the host-managed cores, poll CPA. cAVS 2.5
    // platforms used by SOF's TGL/ADL path expose only core 0 to the host.
    dspPowered = true;
#ifdef OHMYDSP_RPL_LIVE
    const UInt32 hostCores = CORE0;
    LOG("RPL host-managed core mask=0x%x (SOF cAVS 2.5 model)", hostCores);
    U4(ADSPCS, hostCores << 16, hostCores << 16, "RPL ADSPCS: SPA core0");
    if (!poll(b4, ADSPCS, hostCores << 24, hostCores << 24, 50, "RPL CPA core0")) return false;
#else
    const UInt32 hostCores = CORES_ALL;
    U4(ADSPCS, hostCores << 16, hostCores << 16, "ADSPCS: SPA cores0-3");
    if (!poll(b4, ADSPCS, hostCores << 24, hostCores << 24, 50, "CPA cores0-3")) return false;
#endif
    // step 8: ROM_CONTROL
    W4(HIPCIDR, BIT31 | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((UInt32)(STREAM_TAG - 1) << ROM_STREAM_SHIFT), "HIPCIDR: ROM_CONTROL purge+stream");
    // step 9: run core 0 (reset leave, then unstall)
    U4(ADSPCS, CORE0, 0, "ADSPCS: core0 leave reset");
    if (!poll(b4, ADSPCS, CORE0, 0, 50, "CRST0 clear")) return false;
    U4(ADSPCS, CORE0 << 8, 0, "ADSPCS: core0 unstall");
    if (!poll(b4, ADSPCS, (CORE0 << 24) | (CORE0 << 8) | CORE0, CORE0 << 24, 50, "core0 running")) return false;
    // step 10: wait ROM ack
    if (!poll(b4, HIPCIDA, BIT31, BIT31, 500, "HIPCIDA.DONE")) return false;
    U4(HIPCIDA, BIT31, BIT31, "HIPCIDA: clear DONE");
    // step 11: power down non-init host-managed cores. The RPL cAVS 2.5
    // descriptor manages only core 0, so there is no secondary host operation.
#ifdef OHMYDSP_RPL_LIVE
    LOG("RPL secondary core power-down skipped: host-managed mask=0x%x init-core mask=0x%x", hostCores, CORE0);
#else
    U4(ADSPCS, (CORES_ALL & ~CORE0) << 8 | (CORES_ALL & ~CORE0), (CORES_ALL & ~CORE0) << 8 | (CORES_ALL & ~CORE0), "ADSPCS: cores1-3 stall+reset");
    U4(ADSPCS, (CORES_ALL & ~CORE0) << 16, 0, "ADSPCS: cores1-3 SPA off");
    if (!poll(b4, ADSPCS, (CORES_ALL & ~CORE0) << 24, 0, 50, "CPA cores1-3 off")) return false;
#endif
    // step 12: ROM init done
    if (!poll(b4, ROM_STATUS, FSR_MASK, FSR_INIT_DONE, 300, "ROM_STATUS INIT_DONE")) {
        // Seen after system sleep (2026-11-06): the ROM leaves INIT_DONE behind and sits in a CSE state (0x11 = CSE_IMR_GRANTED). Read-only diagnosis:
        // register snapshot, then watch the status for up to 3 s. A late INIT_DONE (slow CSE after resume) is accepted, anything else fails as before.
        romDiag();
        if (!watchRomInitDone(3000)) return false;
    }
    // step 13: start DMA, wait FW_ENTERED, stop
    U0(sdOff, SD_CTL_RUN, SD_CTL_RUN, "SD8 CTL: RUN (code loader DMA start)");
    {
        bool entered = poll(b4, ROM_STATUS, FSR_MASK, FSR_FW_ENTERED, 3000, "ROM_STATUS FW_ENTERED");
        U0(sdOff, SD_CTL_RUN, 0, "SD8 CTL: clear RUN");
        poll(b0, sdOff, SD_CTL_RUN, 0, 5, "SD8 stopped");
        LOG("ROM_STATUS=0x%08x ROM_ERROR=0x%08x LPIB=0x%08x", dry ? 0 : r32(b4, ROM_STATUS), dry ? 0 : r32(b4, ROM_ERROR), dry ? 0 : r32(b0, sdOff + 4));
        if (!entered) return false;
    }
    // step 15: wait FW_READY message (polling, interrupts stay masked)
    if (!poll(b4, HIPCTDR, BIT31, BIT31, 2000, "HIPCTDR.BUSY (FW_READY)")) return false;
    if (!dry) {
        UInt32 hdr[27]; for (int i = 0; i < 27; i++) hdr[i] = r32(b4, MBOX_UPLINK + 4 * i);   // sof_ipc_fw_ready = 108 bytes
        LOG("FW_READY msg: size=%u cmd=0x%08x dspbox=0x%x hostbox=0x%x dspsize=0x%x hostsize=0x%x ver=%u.%u.%u abi=0x%08x",
            hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[7] & 0xffff, hdr[7] >> 16, hdr[8] & 0xffff, hdr[16]);
        ok = (hdr[1] == IPC_FW_READY);
        if (!ok) LOG("unexpected first IPC command 0x%08x (wanted 0x70000000)", hdr[1]);
        // Phase 3 step 3.0 (read-only): dump the mailbox + window heads so the real window layout is known before any IPC is sent.
        if (dumpMailbox && ok) {
            auto rows = [&](const char *what, UInt32 off, UInt32 nwords) {
                for (UInt32 i = 0; i < nwords; i += 8) {
                    UInt32 w[8] = {0}; for (UInt32 k = 0; k < 8 && i + k < nwords; k++) w[k] = r32(b4, off + 4 * (i + k));
                    LOG("P3.0 %s +0x%03x [BAR4 0x%05x]: %08x %08x %08x %08x %08x %08x %08x %08x", what, i * 4, off + 4 * i,
                        w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
            };
            rows("fw_ready+ext (UPBOX)", MBOX_UPLINK, 128);          // 512 bytes: 108 B fw_ready, then extended data
            // walk extended data like ipc3_fw_parse_ext_data (ipc3.c:585): hdr{size,cmd}, type; stops when cmd != FW_READY
            UInt32 off = 108, guard = 0;
            while (off + 12 <= 512 && guard++ < 16) {
                UInt32 esz = r32(b4, MBOX_UPLINK + off), ecmd = r32(b4, MBOX_UPLINK + off + 4), etype = r32(b4, MBOX_UPLINK + off + 8);
                LOG("P3.0 ext[%u] at +%u: size=%u cmd=0x%08x type=%u", guard - 1, off, esz, ecmd, etype);
                if (ecmd != IPC_FW_READY || esz < 12 || off + esz > 512) break;
                off += esz;
            }
            rows("DOWNBOX head", 0xA0000, 16);
            rows("DEBUG head", 0xC0000, 16);
            rows("EXCEPTION head", 0xC0800, 8);
            rows("STREAM head", 0xC1000, 16);
            rows("TRACE head", 0xE0000, 8);
            rows("REGS (ROM_STATUS...)", 0x80000, 8);
        }
    } else ok = true;
    W4(HIPCTDR, BIT31, "HIPCTDR: ack BUSY (w1c)");
    U4(HIPCTDA, BIT31, BIT31, "HIPCTDA: DONE");
    LOG("PHASE2: firmware reports READY: %s", ok ? "YES" : "NO");
    fwReady = ok;
    return ok;
}

// ---- Phase 3 steps 3.1+: replay the first `count` messages of the generated list. One message at a time, each acknowledged and
// checked; the first error / timeout / panic stops (the caller then tears the DSP down). ----
bool DSPDevice::sendPipeline(UInt32 count)
{
    if (!fwReady || !ipcBlob || count < 1) { LOG("P3 skipped: firmware not ready or blob missing"); return false; }
    const UInt8 *bp = (const UInt8 *)ipcBlob->getBytesNoCopy();
    UInt32 nsend = count > ipcCount ? ipcCount : count;
#ifdef OHMYDSP_RPL_LIVE
    if (!ipc4Blob) {
        LOG("P3 ABORT: RPL firmware requires an I4PC IPC4 blob");
        return false;
    }
    UInt32 sentIpc4 = 0;
    for (UInt32 mi = 0; mi < nsend; mi++) {
        UInt32 pos = ipcOff[mi], len = *(const UInt32 *)(bp + pos);
        UInt32 extension = *(const UInt32 *)(bp + pos + 4);
        UInt32 primary = *(const UInt32 *)(bp + pos + 8);
        UInt32 payloadBytes = len - 12;
        const UInt32 *payload = (const UInt32 *)(bp + pos + 12);
        UInt32 replyPrimary = 0, replyExtension = 0;
        if (dry) {
            LOG("DRYRUN IPC4 msg %u/%u: len=%u extension=0x%08x primary=0x%08x payload=%u",
                mi + 1, nsend, len, extension, primary, payloadBytes);
            sentIpc4++;
            continue;
        }
        char what[48]; snprintf(what, sizeof(what), "IPC4 msg %u/%u", mi + 1, nsend);
        if (!ipc4Tx(payload, payloadBytes, primary, extension, what, &replyPrimary, &replyExtension)) return false;
        sentIpc4++;
    }
    LOG("PHASE3 IPC4: %u/%u messages accepted", sentIpc4, nsend);
    return true;
#endif
    if (downbox != 0xA0000 || upbox != MBOX_UPLINK || downSize < 384) {
        LOG("P3 ABORT: manifest mailbox windows unexpected (down=0x%x/0x%x up=0x%x/0x%x), not sending", downbox, downSize, upbox, upSize);
        return false;
    }
    UInt32 sentOk = 0;
    for (UInt32 mi = 0; mi < nsend; mi++) {
        UInt32 len = *(const UInt32 *)(bp + ipcOff[mi]);
        const UInt32 *msg = (const UInt32 *)(bp + ipcOff[mi] + 8);
        UInt32 words = (len + 3) / 4;
        if (dry) {
            LOG("DRYRUN P3 msg %u/%u: len %u cmd 0x%08x w2..5: %08x %08x %08x %08x", mi + 1, nsend, len, msg[1], msg[2], msg[3], msg[4], msg[5]);
            sentOk++; continue;
        }
        UInt32 idr = r32(b4, HIPCIDR), tdr = r32(b4, HIPCTDR);
        if ((idr & BIT31) || (tdr & BIT31)) { LOG("P3 msg %u ABORT: IPC not idle (HIPCIDR=0x%08x HIPCTDR=0x%08x)", mi + 1, idr, tdr); return false; }
        LOG("P3 msg %u/%u: len %u cmd 0x%08x", mi + 1, nsend, len, msg[1]);
        for (UInt32 i = 0; i < words; i++) W4(downbox + 4 * i, msg[i], "P3 mailbox word");
        W4(HIPCIDR, BIT31, "P3 HIPCIDR: BUSY (IPC doorbell)");
        if (!poll(b4, HIPCIDA, BIT31, BIT31, 500, "P3 HIPCIDA.DONE (IPC reply)")) {
            UInt32 w0 = r32(b4, upbox), w1 = r32(b4, downbox);
            LOG("P3 msg %u TIMEOUT: HIPCIDR=0x%08x HIPCIDA=0x%08x HIPCTDR=0x%08x ROM_STATUS=0x%08x ROM_ERROR=0x%08x", mi + 1, r32(b4, HIPCIDR), r32(b4, HIPCIDA), r32(b4, HIPCTDR), r32(b4, ROM_STATUS), r32(b4, ROM_ERROR));
            LOG("P3 upbox[0]=0x%08x downbox[0]=0x%08x panic-magic=%s", w0, w1, ((w0 & 0x0ffff000) == 0x0dead000 || (w1 & 0x0ffff000) == 0x0dead000) ? "YES" : "no");
            for (UInt32 i = 0; i < 16; i += 4)
                LOG("P3 upbox +0x%02x: %08x %08x %08x %08x   exception +0x%02x: %08x %08x %08x %08x", i * 4,
                    r32(b4, upbox + 4 * i), r32(b4, upbox + 4 * i + 4), r32(b4, upbox + 4 * i + 8), r32(b4, upbox + 4 * i + 12), i * 4,
                    r32(b4, 0xC0800 + 4 * i), r32(b4, 0xC0800 + 4 * i + 4), r32(b4, 0xC0800 + 4 * i + 8), r32(b4, 0xC0800 + 4 * i + 12));
            return false;
        }
        // reply is read from the host mailbox (ipc3.c:242): sof_ipc_reply {hdr{size,cmd}, s32 error}; real size can exceed 12
        UInt32 rs = r32(b4, downbox), rc = r32(b4, downbox + 4); SInt32 re = (SInt32)r32(b4, downbox + 8);
        LOG("P3 msg %u reply: size=%u cmd=0x%08x error=%d", mi + 1, rs, rc, re);
        U4(HIPCIDA, BIT31, BIT31, "P3 HIPCIDA: ack DONE");
        UInt32 td = r32(b4, HIPCTDR);
        if (td & BIT31) {   // DSP->host notification: log and ack so the firmware is not stalled
            LOG("P3 DSP notification pending: HIPCTDR=0x%08x upbox: %08x %08x %08x %08x", td, r32(b4, upbox), r32(b4, upbox + 4), r32(b4, upbox + 8), r32(b4, upbox + 12));
            W4(HIPCTDR, BIT31, "P3 HIPCTDR: ack BUSY (w1c)"); U4(HIPCTDA, BIT31, BIT31, "P3 HIPCTDA: DONE");
        }
        if (rs < 12 || re != 0) { LOG("P3 msg %u FAILED (reply size %u error %d): stopping", mi + 1, rs, re); return false; }
        sentOk++;
    }
    LOG("PHASE3: %u/%u messages %s", sentOk, nsend, dry ? "checked (dry run)" : "accepted (error=0)");
    return true;
}

bool DSPDevice::sendIpc4CaptureGraph(UInt32 hostSdi)
{
#ifndef OHMYDSP_RPL_LIVE
    (void)hostSdi;
    return false;
#else
    if (!fwReady || !ipcBlob || !ipc4Blob || hostSdi > 6 || ipcCount != 17) {
        LOG("IPC4 capture graph refused (fw %d blob %d ipc4 %d SD%u messages %u)",
            fwReady, ipcBlob != nullptr, ipc4Blob, hostSdi, ipcCount);
        return false;
    }
    const UInt8 *bp = (const UInt8 *)ipcBlob->getBytesNoCopy();
    // Run 12 appends EQ coefficients after the unchanged run-11 messages.
    // Refuse old/mismatched assets before sending any graph IPC.
    const UInt32 *config = (const UInt32 *)(bp + ipcOff[15]);
    if (config[0] != 12 + 788 || config[1] != 0x30000314u ||
        config[2] != 0x44000014u || config[3] != 788) {
        LOG("IPC4 capture graph refused: expected 788-byte TDFB config for module 20 instance 0");
        return false;
    }
    const UInt32 *eq = (const UInt32 *)(bp + ipcOff[16]);
    if (eq[0] != 12 + 88 || eq[1] != 0x30000058u || eq[2] != 0x4400000au ||
        eq[3] != 88 || eq[4] != 2 || eq[5] != 1) {
        LOG("IPC4 capture graph refused: expected 88-byte two-channel EQ config for module 10 instance 0");
        return false;
    }
    UInt32 sent = 0;
    const UInt32 hostNode = (1u << 8) | hostSdi; // HDA host input gateway, index = stream tag - 1
    LOG("IPC4 capture graph: patch host-copier node_id=0x%08x for SD%u/tag%u", hostNode, hostSdi, hostSdi + 1);
    for (UInt32 mi = 0; mi < ipcCount; mi++) {
        UInt32 pos = ipcOff[mi], len = *(const UInt32 *)(bp + pos);
        UInt32 extension = *(const UInt32 *)(bp + pos + 4);
        UInt32 primary = *(const UInt32 *)(bp + pos + 8);
        UInt32 payloadBytes = len - 12;
        const UInt32 *payload = (const UInt32 *)(bp + pos + 12);
        UInt32 patched[IPC4_MAX_PAYLOAD / sizeof(UInt32)];
        if (payloadBytes > sizeof(patched) || (payloadBytes & 3)) {
            LOG("IPC4 capture graph message %u has invalid payload size %u", mi + 1, payloadBytes);
            return false;
        }
        if (payloadBytes) memcpy(patched, payload, payloadBytes);
        // The generated RPL blob uses the IPC4 MOD_INIT_INSTANCE primary
        // 0x40020004 for the pipeline-11 host copier.  The 0x4102 form is a
        // different target/opcode encoding and is not emitted by this blob.
        if (primary == 0x40020004u) {
            if (payloadBytes < 18 * sizeof(UInt32)) {
                LOG("IPC4 host-copier payload is too short (%u bytes)", payloadBytes);
                return false;
            }
            if (payload[17] != 0x00000100u) {
                LOG("IPC4 host-copier payload node_id baseline unexpected: 0x%08x", payload[17]);
                return false;
            }
            patched[17] = hostNode;
            // Linux IPC4 gives a capture host copier one 1 ms gateway DMA
            // buffer: max(1 ms, deep-buffer-ms) * base_config.ibs.  The
            // topology's IBS is 384 bytes.  The offline replay was built
            // with the DAI copier's 4 ms value (0x600), which is valid for
            // the DMIC gateway but not for this AIF host copier.
            if (payloadBytes < 19 * sizeof(UInt32) || payload[18] != 0x00000600u) {
                LOG("IPC4 host-copier payload DMA size baseline unexpected: 0x%08x", payloadBytes >= 19 * sizeof(UInt32) ? payload[18] : 0);
                return false;
            }
            patched[18] = 0x00000180u;
            LOG("IPC4 host-copier message patched: payload[17]=0x%08x payload[18]=0x%08x (1 ms)", patched[17], patched[18]);
        }
        UInt32 replyPrimary = 0, replyExtension = 0;
        if (dry) {
            LOG("DRYRUN IPC4 capture graph msg %u/%u: primary=0x%08x extension=0x%08x payload=%u",
                mi + 1, ipcCount, primary, extension, payloadBytes);
        } else {
            char what[72];
            if (mi == 15) snprintf(what, sizeof(what), "IPC4 TDFB coefficients 16/17 (788 bytes)");
            else if (mi == 16) snprintf(what, sizeof(what), "IPC4 EQ coefficients 17/17 (88 bytes)");
            else snprintf(what, sizeof(what), "IPC4 capture graph %u/%u", mi + 1, ipcCount);
            if (!ipc4Tx(patched, payloadBytes, primary, extension, what, &replyPrimary, &replyExtension)) return false;
        }
        sent++;
    }
    LOG("IPC4 capture graph: %u/%u messages accepted (TDFB and EQ coefficients included)", sent, ipcCount);
    return sent == ipcCount;
#endif
}

bool DSPDevice::sendIpc4Probe()
{
#ifndef OHMYDSP_RPL_LIVE
    LOG("IPC4 probe unavailable in the IPC3 build");
    return false;
#else
    // GLB_CREATE_PIPELINE for the two empty pipelines used by the reviewed
    // capture plan.  No modules are initialized and the DSP is reset during
    // teardown, so this probe leaves no persistent graph.
    static const UInt32 payload[] = { 0x01000007, 0, 0, 0x00030003, 0, 8192, 24576 };
    UInt32 replyPrimary = 0, replyExtension = 0;
    if (!fwReady) { LOG("IPC4 probe refused: firmware is not ready"); return false; }
    LOG("IPC4 probe: GLB_CREATE_PIPELINE 12 primary=0x%08x extension=0x%08x payload=%u",
        0x11000008u, 0x20000000u, (UInt32)sizeof(payload));
    if (!ipc4Tx(payload, sizeof(payload), 0x11000008u, 0x20000000u,
                "IPC4 GLB_CREATE_PIPELINE 12 probe", &replyPrimary, &replyExtension)) return false;
    LOG("IPC4 probe: GLB_CREATE_PIPELINE 11 primary=0x%08x extension=0x%08x payload=%u",
        0x11010006u, 0x20000000u, (UInt32)sizeof(payload));
    return ipc4Tx(payload, sizeof(payload), 0x11010006u, 0x20000000u,
                  "IPC4 GLB_CREATE_PIPELINE 11 probe", &replyPrimary, &replyExtension);
#endif
}

bool DSPDevice::sendIpc4PipelineState(UInt32 pipelineId, UInt32 state, const char *what)
{
#ifndef OHMYDSP_RPL_LIVE
    (void)pipelineId; (void)state; (void)what;
    return false;
#else
    // IPC4 GLB_SET_PIPELINE_STATE stores the target state in bits 0..15 and
    // the pipeline instance in bits 16..23. A single-pipeline request has no
    // payload and keeps the multi-pipeline extension bit clear.
    if (pipelineId > 0xff || state > 0xffff) {
        LOG("%s ABORT: invalid pipeline state id=%u state=%u", what, pipelineId, state);
        return false;
    }
    UInt32 primary = (state & 0xffffu) | ((pipelineId & 0xffu) << 16) | (19u << 24);
    UInt32 replyPrimary = 0, replyExtension = 0;
    LOG("IPC4 pipeline state: pipeline=%u state=%u primary=0x%08x", pipelineId, state, primary);
    return ipc4Tx(nullptr, 0, primary, 0, what, &replyPrimary, &replyExtension);
#endif
}

bool DSPDevice::sendIpc4PipelineStateMulti(UInt32 state, UInt32 firstPipelineId, UInt32 secondPipelineId,
                                           const char *what)
{
#ifndef OHMYDSP_RPL_LIVE
    (void)state; (void)firstPipelineId; (void)secondPipelineId; (void)what;
    return false;
#else
    if (state > 0xffff || firstPipelineId > 0xff || secondPipelineId > 0xff || firstPipelineId == secondPipelineId) {
        LOG("%s ABORT: invalid multi pipeline state=%u ids=%u,%u", what, state, firstPipelineId, secondPipelineId);
        return false;
    }
    // The payload is a count followed by the runtime pipeline instance IDs.
    // Callers specify the order for this transition. Keep it in one transaction
    // instead of exposing a half-running cross-pipeline graph.
    const UInt32 payload[3] = { 2, firstPipelineId, secondPipelineId };
    UInt32 primary = (state & 0xffffu) | (19u << 24);
    UInt32 replyPrimary = 0, replyExtension = 0;
    LOG("IPC4 multi pipeline state: state=%u ids=%u,%u primary=0x%08x", state, firstPipelineId, secondPipelineId, primary);
    return ipc4Tx(payload, sizeof(payload), primary, 0x00000001u, what, &replyPrimary, &replyExtension);
#endif
}

bool DSPDevice::startIpc4Session()
{
    // The owner has published an immutable copy of the preceding session.
    if (traceArchive) IOFree(traceArchive, TRACE_ARCHIVE_CAPACITY);
    traceArchive = nullptr; traceArchiveBytes = 0; traceSequence = 0;
    tracePhase = 0; traceArchiveStopped = false;
    if (dry || sessionBlocked || sessionTouched || stuck || sdIdx >= 0 || !persistent || !pb.cap) return false;
    if (!checkIdle()) return false;
    sessionTouched = true;
    bool ok = powerUp(false);
    if (ok) ok = beginIpc4TraceCapture();
    if (ok) {
        setIpc4TracePhase(1);
        sessionGraph = true; // partial graph failures also require unwind
        ok = sendIpc4CaptureGraph(5);
    }
    if (ok) ok = sendIpc4PipelineStateMulti(IPC4_STATE_PAUSED, 0, 1, "IPC4 session PAUSED");
    if (ok) ok = setupIpc4CaptureStream(5);
    if (ok) {
        memset(pb.cap->getBytesNoCopy(), 0, capBytes);
        setIpc4TracePhase(2);
        sessionTrace = setIpc4Trace(true);
        ok = sessionTrace;
    }
    if (ok) {
        setIpc4TracePhase(3);
        ok = sendIpc4PipelineStateMulti(IPC4_STATE_RUNNING, 1, 0, "IPC4 session RUNNING");
    }
    if (ok) ok = startIpc4CaptureDma();
    if (!ok) stopIpc4Session();
    LOG("IPC4 session start: %s", ok ? "PASS" : "FAILED");
    return ok;
}

bool DSPDevice::stopIpc4Session()
{
    if (!sessionTouched) return !sessionBlocked;
    bool clean = true;
    setIpc4TracePhase(4);
    if (sessionGraph) clean = sendIpc4PipelineStateMulti(IPC4_STATE_PAUSED, 0, 1, "IPC4 session PAUSE cleanup");
    StopInfo stop;
    if (!stopDma(&stop)) clean = false;
    if (sessionGraph && !sendIpc4PipelineStateMulti(IPC4_STATE_RESET, 1, 0, "IPC4 session RESET cleanup")) clean = false;
    setIpc4TracePhase(5);
    if (sessionTrace && !setIpc4Trace(false)) clean = false;
    setIpc4TracePhase(6);
    bool touched = streamTouchedHw;
    if (!freeStream()) clean = false;
    teardown(false);
    if (touched && !captureDescriptorIdle()) clean = false;
    if (stuck || r32(b4, ADSPCS) != 0x00000f0f) clean = false;
    sessionTouched = sessionGraph = sessionTrace = false;
    if (!clean) sessionBlocked = true; // no retries after an uncertain unwind
    LOG("IPC4 session stop: %s blocked=%d", clean ? "PASS" : "FAILED", sessionBlocked);
    return clean;
}

void DSPDevice::serviceIpc4Trace()
{
    if (!sessionTouched || !fwReady) return;
    drainIpc4Trace();
    for (UInt32 n = 0; n < 4; n++) {
        UInt32 pending = r32(b4, HIPCTDR);
        if (!(pending & BIT31) || !ackIpc4Notification(pending, "IPC4 session async")) break;
    }
}

bool DSPDevice::beginIpc4TraceCapture()
{
    if (dry || !fwReady || stuck || traceArchive || len4 < IPC4_DEBUG + 0x10000) return false;
    traceArchive = (UInt8 *)IOMalloc(TRACE_ARCHIVE_CAPACITY);
    if (!traceArchive) return false;
    // MTR1 archive v1: header [magic, version, flags, full-ring observations].
    // Flags: 1=observed full ring, 2=invalid metadata, 4=archive capacity hit.
    UInt32 header[4] = { 0x3152544d, 1, 0, 0 };
    memcpy(traceArchive, header, sizeof(header));
    traceArchiveBytes = sizeof(header);
    LOG("MTRACE stream begin: capacity=%u; copy before host-read acknowledgement", TRACE_ARCHIVE_CAPACITY);
    drainIpc4Trace();
    return true;
}

void DSPDevice::setIpc4TracePhase(UInt32 phase)
{
    // Drain the preceding phase first, so boundary labels do not relabel it.
    drainIpc4Trace();
    tracePhase = phase;
}

void DSPDevice::drainIpc4Trace()
{
    if (!traceArchive || traceArchiveStopped || dry || !fwReady || len4 < IPC4_DEBUG + 0x10000) return;
    UInt32 *archiveHeader = (UInt32 *)traceArchive;
    UInt32 slots = 0;
    for (UInt32 slot = 0; slot < 15 && slots < 4; slot++) {
        UInt32 type = r32(b4, IPC4_DEBUG + slot * 12 + 4);
        if ((type & 0xffffff00u) != 0x474f4c00u) continue;
        slots++;
        UInt32 base = IPC4_DEBUG + (slot + 1) * 0x1000;
        UInt32 read = r32(b4, base), write = r32(b4, base + 4);
        if (read >= 4088 || write >= 4088) {
            archiveHeader[2] |= 2;
            LOG("MTRACE stream invalid slot=%u read=%u write=%u; no acknowledgement", slot, read, write);
            traceArchiveStopped = true;
            return;
        }
        UInt32 count = (write + 4088 - read) % 4088;
        if (!count) continue;
        if (count == 4087) { archiveHeader[2] |= 1; archiveHeader[3]++; }
        UInt32 padded = (count + 3) & ~3u;
        UInt32 recordBytes = 32 + padded;
        if (recordBytes > TRACE_ARCHIVE_CAPACITY - traceArchiveBytes) {
            archiveHeader[2] |= 4;
            traceArchiveStopped = true;
            LOG("MTRACE stream archive full at %u bytes; unread data not acknowledged", traceArchiveBytes);
            return;
        }
        UInt32 header[8] = { recordBytes, traceSequence, tracePhase, slot, type, read, write, count };
        UInt8 *record = traceArchive + traceArchiveBytes;
        memcpy(record, header, sizeof(header));
        UInt32 lastWord = ~0u, value = 0;
        for (UInt32 i = 0; i < count; i++) {
            UInt32 pos = (read + i) % 4088;
            UInt32 word = pos & ~3u;
            if (word != lastWord) { value = r32(b4, base + 8 + word); lastWord = word; }
            record[32 + i] = (UInt8)(value >> ((pos & 3) * 8));
        }
        memset(record + 32 + count, 0, padded - count);
        // Producer never overwrites unread bytes (Zephyr mtrace_out). It may
        // append while copying; acknowledge only the captured write pointer.
        // Metadata/read-pointer changes imply reset or another consumer.
        if (r32(b4, base) != read || r32(b4, IPC4_DEBUG + slot * 12 + 4) != type) {
            archiveHeader[2] |= 2;
            traceArchiveStopped = true;
            LOG("MTRACE stream slot changed during copy; no acknowledgement");
            return;
        }
        traceArchiveBytes += recordBytes;
        traceSequence++;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        W4(base, write, "MTRACE host read: copied bytes retained in archive");
    }
}

bool DSPDevice::setIpc4Trace(bool enable)
{
    if (dry || !fwReady || stuck) return false;
    // Linux ipc4-mtrace.c / SOF 2.12 ipc4_log_state_info: 19 DWORDs.
    // module 0, instance 0, LARGE_CONFIG_SET, parameter 6, first+last.
    UInt32 payload[19] = { 0x100, 0x1000, enable ? 1u : 0u, 0x1ef };
    return ipc4Tx(payload, sizeof(payload), 0x44000000, 0x3060004c,
                  enable ? "IPC4 mtrace enable" : "IPC4 mtrace disable", nullptr, nullptr);
}

bool DSPDevice::ackIpc4Notification(UInt32 primary, const char *what)
{
    // Global request type 27, distinct from a module/global command reply.
    if ((primary & 0x7f000000u) != 0x1b000000u) return false;
    drainIpc4Trace();
    LOG("%s notification: primary=%08x extension=%08x type=%u core=%u uplink=%08x %08x %08x %08x",
        what, primary & ~BIT31, r32(b4, HIPCTDD), (primary >> 16) & 0xff,
        (primary >> 12) & 0xf, r32(b4, upbox), r32(b4, upbox+4), r32(b4, upbox+8), r32(b4, upbox+12));
    W4(HIPCTDR, BIT31, "IPC4 notification: ack BUSY (w1c)");
    U4(HIPCTDA, BIT31, BIT31, "IPC4 notification: DONE");
    return true;
}

bool DSPDevice::ipc4Tx(const UInt32 *payload, UInt32 payloadBytes, UInt32 primary, UInt32 extension,
                       const char *what, UInt32 *replyPrimary, UInt32 *replyExtension)
{
#ifndef OHMYDSP_RPL_LIVE
    (void)payload; (void)payloadBytes; (void)primary; (void)extension; (void)what;
    (void)replyPrimary; (void)replyExtension;
    return false;
#else
    if ((payloadBytes && !payload) || (payloadBytes & 3) || payloadBytes > IPC4_MAX_PAYLOAD ||
        !downbox || !upbox || downSize < IPC4_MAX_PAYLOAD || upSize < IPC4_MAX_PAYLOAD) {
        LOG("%s ABORT: invalid IPC4 mailbox payload/windows (payload=%u down=0x%x/0x%x up=0x%x/0x%x)",
            what, payloadBytes, downbox, downSize, upbox, upSize);
        return false;
    }
    UInt32 notifications = 0;
    drainIpc4Trace();
    // A notification can already be pending between transactions. Drain only
    // notifications; an unsolicited reply remains an error. Bound all phases.
    for (UInt32 n = 0; n < 16; n++) {
        UInt32 pending = r32(b4, HIPCTDR);
        if (!(pending & BIT31) || !ackIpc4Notification(pending, what)) break;
        notifications++;
        IOSleep(1);
    }
    UInt32 idr = r32(b4, HIPCIDR), tdr = r32(b4, HIPCTDR), ida = r32(b4, HIPCIDA);
    LOG("%s pre: HIPCIDR=0x%08x HIPCIDD=0x%08x HIPCIDA=0x%08x HIPCTDR=0x%08x HIPCTDD=0x%08x",
        what, idr, r32(b4, HIPCIDD), ida, tdr, r32(b4, HIPCTDD));
    if (tdr & BIT31 || idr & BIT31) {
        LOG("%s ABORT: IPC busy before send", what);
        return false;
    }
    if (ida & BIT31) U4(HIPCIDA, BIT31, BIT31, "IPC4 HIPCIDA: clear stale DONE");
    for (UInt32 i = 0; i < payloadBytes / 4; i++) W4(downbox + 4 * i, payload[i], "IPC4 outbox payload");
    W4(HIPCIDD, extension, "IPC4 HIPCIDD: extension");
    W4(HIPCIDR, primary | BIT31, "IPC4 HIPCIDR: primary+BUSY");
    for (UInt32 ms = 0; ms < 500 && !(r32(b4, HIPCIDA) & BIT31); ms++) {
        drainIpc4Trace();
        UInt32 pending = r32(b4, HIPCTDR);
        if ((pending & BIT31) && notifications < 32 && ackIpc4Notification(pending, what)) notifications++;
        IOSleep(1);
    }
    if (!(r32(b4, HIPCIDA) & BIT31)) {
        LOG("%s accept timeout: HIPCIDR=0x%08x HIPCIDA=0x%08x HIPCTDR=0x%08x HIPCCTL=0x%08x",
            what, r32(b4, HIPCIDR), r32(b4, HIPCIDA), r32(b4, HIPCTDR), r32(b4, 0xE8));
        return false;
    }
    LOG("%s accepted: HIPCIDR=0x%08x HIPCIDA=0x%08x", what, r32(b4, HIPCIDR), r32(b4, HIPCIDA));
    U4(HIPCIDA, BIT31, BIT31, "IPC4 HIPCIDA: ack DONE");
    bool haveReply = false;
    for (UInt32 ms = 0; ms < 500; ms++) {
        drainIpc4Trace();
        UInt32 pending = r32(b4, HIPCTDR);
        if (pending & BIT31) {
            if ((pending & 0x7f000000u) == 0x1b000000u) {
                if (notifications >= 32) { LOG("%s ABORT: notification limit", what); return false; }
                ackIpc4Notification(pending, what);
                notifications++;
            } else { haveReply = true; break; }
        }
        IOSleep(1);
    }
    if (!haveReply) {
        LOG("%s reply timeout: HIPCIDR=0x%08x HIPCIDA=0x%08x HIPCTDR=0x%08x HIPCTDD=0x%08x",
            what, r32(b4, HIPCIDR), r32(b4, HIPCIDA), r32(b4, HIPCTDR), r32(b4, HIPCTDD));
        for (UInt32 i = 0; i < 4; i++) LOG("%s uplink[%u]=0x%08x", what, i, r32(b4, upbox + 4 * i));
        return false;
    }
    UInt32 rp = r32(b4, HIPCTDR) & ~BIT31;
    UInt32 re = r32(b4, HIPCTDD) & ~BIT31;
    if (replyPrimary) *replyPrimary = rp;
    if (replyExtension) *replyExtension = re;
    LOG("%s reply: primary=0x%08x extension=0x%08x status=%u uplink[0..3]=%08x %08x %08x %08x",
        what, rp, re, rp & IPC4_REPLY_STATUS, r32(b4, upbox), r32(b4, upbox + 4),
        r32(b4, upbox + 8), r32(b4, upbox + 12));
    W4(HIPCTDR, BIT31, "IPC4 HIPCTDR: ack BUSY (w1c)");
    U4(HIPCTDA, BIT31, BIT31, "IPC4 HIPCTDA: DONE");
    drainIpc4Trace();
    if (!(rp & IPC4_REPLY_DIR) || (rp & 0x5f000000u) != (primary & 0x5f000000u)) {
        LOG("%s FAILED: mismatched reply direction/target/type (primary=0x%08x request=0x%08x)", what, rp, primary);
        return false;
    }
    if (rp & IPC4_REPLY_STATUS) {
        LOG("%s FAILED: firmware status=%u", what, rp & IPC4_REPLY_STATUS);
        return false;
    }
    return true;
#endif
}

// one IPC round trip for stream messages (same transport as the Phase 3 replay)
bool DSPDevice::ipcTx(const UInt32 *msg, UInt32 len, const char *what, UInt32 minReply, UInt32 *reply)
{
    UInt32 idr = r32(b4, HIPCIDR), tdr = r32(b4, HIPCTDR);
    if ((idr & BIT31) || (tdr & BIT31)) { LOG("P4 %s ABORT: IPC not idle (HIPCIDR=0x%08x HIPCTDR=0x%08x)", what, idr, tdr); return false; }
    LOG("P4 %s: len %u cmd 0x%08x", what, len, msg[1]);
    for (UInt32 i = 0; i < (len + 3) / 4; i++) W4(downbox + 4 * i, msg[i], "P4 mailbox word");
    W4(HIPCIDR, BIT31, "P4 HIPCIDR: BUSY (IPC doorbell)");
    if (!poll(b4, HIPCIDA, BIT31, BIT31, 500, "P4 HIPCIDA.DONE (IPC reply)")) {
        UInt32 w0 = r32(b4, upbox), w1 = r32(b4, downbox);
        LOG("P4 %s TIMEOUT: HIPCIDA=0x%08x HIPCTDR=0x%08x ROM_STATUS=0x%08x upbox[0]=0x%08x downbox[0]=0x%08x panic-magic=%s", what, r32(b4, HIPCIDA), r32(b4, HIPCTDR),
            r32(b4, ROM_STATUS), w0, w1, ((w0 & 0x0ffff000) == 0x0dead000 || (w1 & 0x0ffff000) == 0x0dead000) ? "YES" : "no");
        return false; }
    for (int i = 0; i < 5; i++) reply[i] = r32(b4, downbox + 4 * i);
    LOG("P4 %s reply: size=%u cmd=0x%08x error=%d w3=0x%08x w4=0x%08x", what, reply[0], reply[1], (SInt32)reply[2], reply[3], reply[4]);
    U4(HIPCIDA, BIT31, BIT31, "P4 HIPCIDA: ack DONE");
    if (r32(b4, HIPCTDR) & BIT31) { LOG("P4 DSP notification pending: upbox %08x %08x %08x", r32(b4, upbox), r32(b4, upbox + 4), r32(b4, upbox + 8));
        W4(HIPCTDR, BIT31, "P4 HIPCTDR: ack BUSY (w1c)"); U4(HIPCTDA, BIT31, BIT31, "P4 HIPCTDA: DONE"); }
    if (reply[0] < minReply || (SInt32)reply[2] != 0) { LOG("P4 %s FAILED (reply size %u error %d)", what, reply[0], (SInt32)reply[2]); return false; }
    return true;
}

bool DSPDevice::setupIpc4CaptureStream(UInt32 sdi)
{
    if (dry || !fwReady || stuck || sdIdx >= 0 || sdi > 6) {
        LOG("IPC4 capture stream refused (dry %d fw %d stuck %d active %d SD%u)",
            dry, fwReady, stuck, sdIdx >= 0, sdi);
        return false;
    }
    sd = SD_BASE + SD_STRIDE * sdi;
    sdBit = 1u << sdi;
    sdTag = sdi + 1;
    sdIdx = (int)sdi;
    streamMode = 6;
    streamFailed = false;
    paramsSent = false;
    dmaOn = false;
    triggered = false;
    streamTouchedHw = false;
    LOG("IPC4 capture stream setup: SD%u tag %u, no PCM_PARAMS, no RUN", sdi, sdTag);

    UInt32 ctl = r32(b0, sd);
    if ((ctl & SD_CTL_RUN) || ((ctl >> SD_TAG_SHIFT) & 0xF) || r32(b0, sd + SD_CBL) ||
        (r32(b0, ppBase + 4) & sdBit) || (r32(b0, spibBase + 4) & sdBit)) {
        LOG("IPC4 capture stream abort: SD%u not idle/unused (CTL=0x%08x)", sdi, ctl);
        sdIdx = -1;
        return false;
    }
    if (!pb.cap && !allocBuffers(pb)) {
        streamFailed = true;
        return false;
    }
    memset(pb.cap->getBytesNoCopy(), 0xA5, capBytes);

    streamTouchedHw = true;
    U0(ppBase + 4, sdBit, sdBit, "IPC4 PPCTL: decouple capture SD");
    U0(sd, SD_CTL_RUN | SD_CTL_INTS, 0, "IPC4 SD CTL: clear RUN/int enables");
    if (!poll(b0, sd, SD_CTL_RUN, 0, 5, "IPC4 SD RUN clear")) { streamFailed = true; return false; }
    W0_8(sd + SD_STS, SD_CTL_INTS, "IPC4 SD STS clear (w1c)");
    U0(sd, SD_CTL_SRST, SD_CTL_SRST, "IPC4 SD CTL: enter reset");
    if (!poll(b0, sd, SD_CTL_SRST, SD_CTL_SRST, 5, "IPC4 SD in reset")) { streamFailed = true; return false; }
    U0(sd, SD_CTL_SRST, 0, "IPC4 SD CTL: leave reset");
    if (!poll(b0, sd, SD_CTL_SRST, 0, 5, "IPC4 SD out of reset")) { streamFailed = true; return false; }
    W0(sd + SD_BDLPL, 0, "IPC4 BDLPL clear");
    W0(sd + SD_BDLPU, 0, "IPC4 BDLPU clear");
    U0(sd, 0xF << SD_TAG_SHIFT, sdTag << SD_TAG_SHIFT, "IPC4 SD CTL: stream tag");
    W0(sd + SD_CBL, capBytes, "IPC4 SD CBL");
    W0_16(sd + SD_FMT, P4_FMT, "IPC4 SD FORMAT (48k, 32 bit, 2 ch)");
    W0_16(sd + SD_LVI, (UInt16)(pb.nent - 1), "IPC4 SD LVI");
    W0(sd + SD_BDLPL, (UInt32)pb.bdlIova, "IPC4 BDLPL");
    W0(sd + SD_BDLPU, (UInt32)(pb.bdlIova >> 32), "IPC4 BDLPU");
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    LOG("IPC4 capture stream ready: buffer=%u BDL entries=%u page-table IOVA=0x%llx",
        capBytes, pb.nent, pb.ptIova);
    return true;
}

bool DSPDevice::startIpc4CaptureDma()
{
    if (dry || sdIdx < 0 || streamMode != 6 || dmaOn) {
        LOG("IPC4 capture DMA start refused (dry %d active %d mode %d dmaOn %d)",
            dry, sdIdx >= 0, streamMode, dmaOn);
        return false;
    }
    UInt32 lp0 = r32(b0, sd + 4);
    UInt32 sts0 = r32(b0, sd) >> 24;
    LOG("IPC4 capture before SD RUN: LPIB=0x%08x STS=0x%02x CTL=0x%08x", lp0, sts0, r32(b0, sd));
    if (sts0 & 0x18) { streamFailed = true; return false; }
    ipc4LpibStart = lp0;
    clock_get_uptime(&ipc4DmaStartTime);
    dmaOn = true;
    U0(sd, SD_CTL_RUN, SD_CTL_RUN, "IPC4 SD CTL: RUN (DMA start)");
    if (!poll(b0, sd, SD_CTL_RUN, SD_CTL_RUN, 5, "IPC4 SD RUN set")) { streamFailed = true; return false; }
    LOG("IPC4 capture SD RUN set: LPIB=0x%08x STS=0x%02x CTL=0x%08x",
        r32(b0, sd + 4), r32(b0, sd) >> 24, r32(b0, sd));
    return true;
}

bool DSPDevice::observeIpc4CaptureStream(UInt32 durationMs)
{
    if (dry || sdIdx < 0 || streamMode != 6 || !dmaOn || durationMs == 0 || durationMs > 2000) {
        LOG("IPC4 capture observe refused (dry %d active %d mode %d dmaOn %d duration %u)",
            dry, sdIdx >= 0, streamMode, dmaOn, durationMs);
        return false;
    }
    UInt32 lp0 = ipc4LpibStart;

    bool wrapped = false;
    UInt32 lpLast = lp0, lpMax = lp0, error = 0;
    UInt32 marks[] = {5, 10, 25, 50, 100, 250, 500, 1000};
    UInt32 nextMark = 0;
    for (UInt32 elapsed = 0; elapsed < durationMs; elapsed += 5) {
        IOSleep(5);
        drainIpc4Trace();
        UInt32 lp = 0, sts = 0;
        sample(&lp, &sts);
        if (lp < lpLast || lp >= capBytes) wrapped = true;
        if (lp > lpMax) lpMax = lp;
        if (sts & 0x18) error |= sts;
        while (nextMark < sizeof(marks) / sizeof(marks[0]) && elapsed + 5 >= marks[nextMark]) {
            LOG("IPC4 capture t=%u ms: LPIB=0x%08x STS=0x%02x", marks[nextMark], lp, sts);
            nextMark++;
        }
        lpLast = lp;
    }
    UInt64 now, elapsedNs;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now - ipc4DmaStartTime, &elapsedNs);
    const bool valid = !wrapped && lpLast > lp0 && !error &&
                       elapsedNs < (UInt64)capBytes * 1000000000ULL / 384000;
    LOG("IPC4 capture: frames=%u start=%u end=%u max=%u elapsed-ms=%llu SD-errors=0x%x %s",
        valid ? (lpLast - lp0) / 8 : 0, lp0, lpLast, lpMax, elapsedNs / 1000000,
        error, valid ? "PASS" : "FAILED");
    if (!valid) streamFailed = true;
    return valid;
}

bool DSPDevice::captureDescriptorIdle()
{
    UInt32 ctl = r32(b0, sd) & (SD_CTL_RUN | SD_CTL_SRST | (0xfu << SD_TAG_SHIFT));
    UInt32 cbl = r32(b0, sd + SD_CBL);
    UInt32 lo = r32(b0, sd + SD_BDLPL), hi = r32(b0, sd + SD_BDLPU);
    UInt32 decoupled = r32(b0, ppBase + 4) & sdBit;
    if (!(ctl || cbl || lo || hi || decoupled)) return true;
    LOG("SD5 cleanup failed: CTL=%x CBL=%x BDL=%x:%x PPCTL=%x", ctl, cbl, hi, lo, decoupled);
    return false;
}

bool DSPDevice::finishIpc4CaptureProbe(bool graph, bool trace)
{
    setIpc4TracePhase(4);
    bool paused = !graph || sendIpc4PipelineStateMulti(IPC4_STATE_PAUSED, 0, 1, "IPC4 probe PAUSE cleanup");
    StopInfo stop;
    bool stopped = stopDma(&stop) && !(stop.stsEnd & 0x18);
    bool reset = !graph || sendIpc4PipelineStateMulti(IPC4_STATE_RESET, 1, 0, "IPC4 probe RESET cleanup");
    setIpc4TracePhase(5);
    bool disabled = !trace || setIpc4Trace(false);
    setIpc4TracePhase(6);
    bool touched = streamTouchedHw;
    bool freed = freeStream();
    teardown(false);
    bool idle = checkIdle();
    if (touched && !captureDescriptorIdle()) idle = false;
    LOG("IPC4 probe cleanup: paused=%d stopped=%d reset=%d trace-disabled=%d freed=%d idle=%d",
        paused, stopped, reset, disabled, freed, idle);
    return paused && stopped && reset && disabled && freed && idle;
}

bool DSPDevice::runIpc4CaptureStream(UInt32 durationMs)
{
    if (!startIpc4CaptureDma()) return false;
    return observeIpc4CaptureStream(durationMs);
}

// ---- Phase 4 capture stream: program stream SDn, send PCM_PARAMS, optionally DMA RUN and TRIG_START ----
// mode 2: no RUN, no TRIG_START; 3: DMA RUN, no TRIG_START; 4/5: DMA RUN then TRIG_START (Linux order, sof_pcm_trigger IPC3).
// Mirrors hda_dsp_stream_trigger minus INTCTL and the SD interrupt enables (polling only).
bool DSPDevice::startStream(int mode, UInt32 sdi, UInt8 fill)
{
    if (dry || !fwReady || stuck || sdIdx >= 0 || mode < 2 || mode > 5 || sdi > 6 || ipcCount < 19) {
        LOG("P4 start refused (dry %d fw %d stuck %d active %d mode %d sd %u blob msgs %u)", dry, fwReady, stuck, sdIdx >= 0, mode, sdi, ipcCount); return false; }
    sd = SD_BASE + SD_STRIDE * sdi; sdBit = 1u << sdi; sdTag = sdi + 1; sdIdx = (int)sdi; streamMode = mode;
    streamFailed = false; paramsSent = false; dmaOn = false; triggered = false; streamTouchedHw = false;
    LOG("PHASE4.%d start: SD%u tag %u, %s", mode, sdi, sdTag, mode == 2 ? "no RUN, no TRIG_START" : mode == 3 ? "DMA RUN 100 ms, no TRIG_START" : mode == 4 ? "DMA RUN + TRIG_START for 500 ms" : "DMA RUN + TRIG_START for 2000 ms, raw capture to file");
    UInt32 ctl = r32(b0, sd);
    if ((ctl & SD_CTL_RUN) || ((ctl >> SD_TAG_SHIFT) & 0xF) || r32(b0, sd + SD_CBL) || (r32(b0, ppBase + 4) & sdBit) || (r32(b0, spibBase + 4) & sdBit)) {
        LOG("P4.2 ABORT: SD%u not idle/unused (CTL=0x%08x)", sdi, ctl); sdIdx = -1; return false; }
    if (!pb.cap) { if (!allocBuffers(pb)) { streamFailed = true; return false; } }
    else memset(pb.cap->getBytesNoCopy(), fill, capBytes);   // persistent ring: start from a known content (0 = silence for the audio engine)
    // host side first (hda_dsp_stream_hw_params order), then PCM_PARAMS (pcm.c)
    streamTouchedHw = true;
    U0(ppBase + 4, sdBit, sdBit, "P4 PPCTL: decouple capture SD");
    U0(sd, SD_CTL_RUN | SD_CTL_INTS, 0, "P4 SD CTL: clear RUN/int enables");
    if (!poll(b0, sd, SD_CTL_RUN, 0, 5, "P4 SD RUN clear")) { streamFailed = true; return false; }
    W0_8(sd + SD_STS, SD_CTL_INTS, "P4 SD STS clear (w1c)");
    U0(sd, SD_CTL_SRST, SD_CTL_SRST, "P4 SD CTL: enter reset");
    if (!poll(b0, sd, SD_CTL_SRST, SD_CTL_SRST, 5, "P4 SD in reset")) { streamFailed = true; return false; }
    U0(sd, SD_CTL_SRST, 0, "P4 SD CTL: leave reset");
    if (!poll(b0, sd, SD_CTL_SRST, 0, 5, "P4 SD out of reset")) { streamFailed = true; return false; }
    W0(sd + SD_BDLPL, 0, "P4 BDLPL clear"); W0(sd + SD_BDLPU, 0, "P4 BDLPU clear");
    U0(sd, 0xF << SD_TAG_SHIFT, sdTag << SD_TAG_SHIFT, "P4 SD CTL: stream tag");
    W0(sd + SD_CBL, capBytes, "P4 SD CBL");
    W0_16(sd + SD_FMT, P4_FMT, "P4 SD FORMAT (48k, 32 bit, 2 ch)");
    W0_16(sd + SD_LVI, (UInt16)(pb.nent - 1), "P4 SD LVI");
    W0(sd + SD_BDLPL, (UInt32)pb.bdlIova, "P4 BDLPL"); W0(sd + SD_BDLPU, (UInt32)(pb.bdlIova >> 32), "P4 BDLPU");
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // PCM_PARAMS (sof_ipc_pcm_params, 108 B; layout compile-checked against include/sound/sof/stream.h)
    UInt32 pp[27] = {0}, reply[5] = {0};
    pp[0] = 108; pp[1] = 0x60010000; pp[2] = P4_COMP_HOST;
    pp[6] = 84;                                   // params.hdr.size
    pp[8] = (UInt32)pb.ptIova; pp[9] = capPages; pp[10] = capBytes;   // buffer.phy_addr / pages / size
    pp[14] = 1; pp[15] = 2; pp[16] = 0; pp[17] = 48000;            // direction capture, S32_LE, interleaved, rate
    pp[18] = sdTag | (2u << 16);                  // stream_tag, channels
    pp[19] = 4u | (4u << 16);                     // valid bytes, container bytes
    pp[20] = P4_PERIOD;                           // host_period_bytes
    pp[21] = 1;                                   // no_stream_position = 1, cont_update_posn = 0
    if (!ipcTx(pp, 108, "PCM_PARAMS", 20, reply)) { streamFailed = true; return false; }
    paramsSent = true;
    if (reply[4] % 76 != 0 || reply[4] > 0x2000) { LOG("P4 PCM_PARAMS reply: bad posn_offset %u", reply[4]); streamFailed = true; return false; }
    if (mode >= 3) {
        UInt32 lp0 = r32(b0, sd + 4), sts0 = r32(b0, sd) >> 24, rp[5] = {0};
        LOG("P4.%d before RUN: LPIB=0x%08x STS=0x%02x CTL=0x%08x", mode, lp0, sts0, r32(b0, sd));
        dmaOn = true;
        U0(sd, SD_CTL_RUN, SD_CTL_RUN, "P4 SD CTL: RUN (DMA start)");
        if (!poll(b0, sd, SD_CTL_RUN, SD_CTL_RUN, 5, "P4 SD RUN set")) { streamFailed = true; return false; }
        if (mode >= 4) {
            UInt32 st[3] = {12, 0x60040000, P4_COMP_HOST};
            if (!ipcTx(st, 12, "TRIG_START", 12, rp)) { streamFailed = true; return false; }
            triggered = true;
        }
    }
    return true;
}

void DSPDevice::sample(UInt32 *lpib, UInt32 *sts)
{
    *lpib = r32(b0, sd + 4); *sts = r32(b0, sd) >> 24;
}

// Stop order (Linux sof_pcm_trigger): TRIG_STOP first, then clear RUN. Idempotent. A RUN bit that will not clear marks the stream
// stuck: freeStream() then leaks the buffers on purpose (a running DMA engine must never lose its target memory).
bool DSPDevice::stopDma(StopInfo *info)
{
    StopInfo si;
    if (!dmaOn) { si.stopped = true; si.ok = true; if (info) *info = si; return true; }
    bool txok = true;
    if (triggered) {
        UInt32 sp[3] = {12, 0x60050000, P4_COMP_HOST}, rp[5] = {0};
        if (!ipcTx(sp, 12, "TRIG_STOP", 12, rp)) txok = false;
        triggered = false;
    }
    U0(sd, SD_CTL_RUN, 0, "P4 SD CTL: clear RUN (DMA stop)");
    si.stopped = poll(b0, sd, SD_CTL_RUN, 0, 5, "P4 SD RUN clear");
    si.stsEnd = r32(b0, sd) >> 24; si.lpEnd = r32(b0, sd + 4); si.ctlEnd = r32(b0, sd);
    W0_8(sd + SD_STS, SD_CTL_INTS, "P4 SD STS clear (w1c)");
    si.ok = si.stopped && txok && !(si.stsEnd & 0x10);
    if (si.stopped) dmaOn = false; else stuck = true;
    if (!si.ok) streamFailed = true;
    if (info) *info = si;
    return si.ok;
}

bool DSPDevice::freeStream()
{
    if (sdIdx < 0 && !pb.cap && !streamTouchedHw) return true;
    if (dmaOn || triggered) stopDma(nullptr);
    if (paramsSent && !streamFailed) {   // PCM_FREE (not after a failure, as in Phase 4)
        UInt32 fr[3] = {12, 0x60030000, P4_COMP_HOST}, reply[5] = {0};
        if (!ipcTx(fr, 12, "PCM_FREE", 12, reply)) streamFailed = true;
    }
    paramsSent = false;
    if (streamTouchedHw) {   // undo the stream setup
        U0(sd, SD_CTL_RUN | SD_CTL_INTS, 0, "P4 undo: clear RUN/int enables");
        poll(b0, sd, SD_CTL_RUN, 0, 5, "P4 undo: RUN clear");
        U0(sd, SD_CTL_SRST, SD_CTL_SRST, "P4 undo: stream reset"); poll(b0, sd, SD_CTL_SRST, SD_CTL_SRST, 5, "P4 undo: in reset");
        U0(sd, SD_CTL_SRST, 0, "P4 undo: leave reset"); poll(b0, sd, SD_CTL_SRST, 0, 5, "P4 undo: out of reset");
        W0(sd + SD_BDLPL, 0, "P4 undo: BDLPL"); W0(sd + SD_BDLPU, 0, "P4 undo: BDLPU"); W0(sd + SD_CBL, 0, "P4 undo: CBL");
        W0_16(sd + SD_FMT, 0, "P4 undo: FORMAT"); W0_16(sd + SD_LVI, 0, "P4 undo: LVI");
        U0(sd, 0xF << SD_TAG_SHIFT, 0, "P4 undo: tag");
        U0(ppBase + 4, sdBit, 0, "P4 undo: PPCTL couple");
        if (!dry) LOG("P4.2 SD%d after undo: CTL=0x%08x CBL=0x%08x PPCTL=0x%08x", sdIdx, r32(b0, sd), r32(b0, sd + SD_CBL), r32(b0, ppBase + 4));
        if (r32(b0, sd) & SD_CTL_RUN) stuck = true;
        streamTouchedHw = false;
    }
    if (stuck) { LOG("P4 ABORT: SD%d RUN could not be cleared: capture buffers are deliberately leaked", sdIdx); pb = Buf(); }
    if (!persistent) freeBuffers(pb);
    bool clean = !streamFailed && !stuck;
    dmaOn = false; triggered = false; sdIdx = -1;
    return clean;
}

// ---- step 14/16/17: teardown in reverse (also the failure path). Reads of ADSPCS are safe; writes are logged. ----
void DSPDevice::teardown(bool keepDspPowered)
{
    if (keepDspPowered) { LOG("ohmydsp_p2_keep set: leaving the DSP running"); dspPowered = false; }
    if (streamTouched) {
        const UInt32 s = sdOff, bit = 1u << STREAM_INDEX;
        U0(s, SD_CTL_RUN, 0, "teardown: SD8 clear RUN");
        U0(s, SD_CTL_SRST, SD_CTL_SRST, "teardown: SD8 reset"); poll(b0, s, SD_CTL_SRST, SD_CTL_SRST, 5, "SD8 in reset");
        U0(s, SD_CTL_SRST, 0, "teardown: SD8 leave reset"); poll(b0, s, SD_CTL_SRST, 0, 5, "SD8 out of reset");
        W0(s + SD_BDLPL, 0, "teardown: BDLPL"); W0(s + SD_BDLPU, 0, "teardown: BDLPU"); W0(s + SD_CBL, 0, "teardown: CBL");
        W0(spibBase + 0x08 + 0x08 * STREAM_INDEX, 0, "teardown: SPIB[8]=0");
        U0(spibBase + 4, bit, 0, "teardown: SPBFCCTL disable SD8");
        U0(ppBase + 4, bit, 0, "teardown: PPCTL couple SD8");
        streamTouched = false;
    }
    if (dspPowered) {
        U4(ADSPCS, CORES_ALL << 8 | CORES_ALL, CORES_ALL << 8 | CORES_ALL, "power down: stall+reset all cores");
        U4(ADSPCS, CORES_ALL << 16, 0, "power down: SPA off");
        poll(b4, ADSPCS, CORES_ALL << 24, 0, 50, "CPA all off");
        if (!dry) LOG("final ADSPCS=0x%08x (idle baseline was 0x00000f0f)", r32(b4, ADSPCS));
        dspPowered = false; fwReady = false;
    }
    if (gatingChanged) {
        WC(PCI_PGCTL, savedPG, "restore PGCTL"); W0(EM2, savedEM2, "restore EM2"); WC(PCI_CGCTL, savedCG, "restore CGCTL");
        gatingChanged = false;
    }
}
