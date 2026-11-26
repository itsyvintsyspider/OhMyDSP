#!/usr/bin/env python3
"""Exercise actual kernel ring-consumption methods against scripted SRAM."""
import subprocess
import tempfile
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'ohmydsp/src/DSPDevice.cpp').read_text()
# Extract the named definitions through their top-level closing braces.
pattern = r'^(?:bool|void) DSPDevice::(?:beginIpc4TraceCapture|setIpc4TracePhase|drainIpc4Trace)\(.*?^\}$'
methods = '\n'.join(re.findall(pattern, source, re.MULTILINE | re.DOTALL)) + '\n'
harness = r'''
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
using UInt32 = uint32_t;
using UInt8 = uint8_t;
#define LOG(...) do {} while (0)
enum : UInt32 { IPC4_DEBUG=0xc0000 };
static UInt32 ram[0x100000/4];
static unsigned writes, mutate;
static void *IOMalloc(unsigned n) { return std::malloc(n); }
static UInt32 r32(volatile UInt8*, UInt32 offset) {
    assert(offset%4==0 && offset<0x100000);
    if (mutate && offset==0xc1008) {
        if (mutate==1) ram[0xc1004/4]=8; // producer appends while consumer copies
        if (mutate==2) ram[0xc1000/4]=1; // another consumer/reset invalidates ownership
        if (mutate==3) ram[0xc0004/4]=0; // descriptor removed
        mutate=0;
    }
    return ram[offset/4];
}
class DSPDevice {
public:
    bool dry=false, fwReady=true, stuck=false;
    volatile UInt8 *b4=nullptr;
    size_t len4=0x100000;
    enum : UInt32 { TRACE_ARCHIVE_CAPACITY=65536 };
    UInt8 *traceArchive=nullptr;
    UInt32 traceArchiveBytes=0, traceSequence=0, tracePhase=0;
    bool traceArchiveStopped=false;
    ~DSPDevice() { std::free(traceArchive); }
    void W4(UInt32 offset, UInt32 value, const char*) {
        // Only host read pointers are writable; complete archived data must
        // already exist when acknowledging the corresponding producer pointer.
        assert(offset>=0xc1000 && offset<=0xcf000 && offset%4096==0);
        assert(traceSequence && traceArchiveBytes>=48);
        unsigned pos=16;
        for(unsigned n=1;n<traceSequence;n++) pos+=*(UInt32*)(traceArchive+pos);
        UInt32 *last=(UInt32*)(traceArchive+pos);
        assert(last[6]==value && pos+last[0]==traceArchiveBytes);
        ram[offset/4]=value; writes++;
    }
    bool beginIpc4TraceCapture();
    void setIpc4TracePhase(UInt32);
    void drainIpc4Trace();
};
'''
cases = r'''
static void reset(UInt32 read=0, UInt32 write=0) {
    std::memset(ram,0,sizeof(ram)); writes=0; mutate=0;
    ram[0xc0004/4]=0x474f4c00; ram[0xc1000/4]=read; ram[0xc1004/4]=write;
    auto bytes=(UInt8*)ram;
    for(unsigned i=0;i<4088;i++) bytes[0xc1008+i]=i%251;
}
static UInt32 flags(DSPDevice& d) { return ((UInt32*)d.traceArchive)[2]; }
int main() {
    { reset(); DSPDevice d; assert(d.beginIpc4TraceCapture()); assert(writes==0 && d.traceArchiveBytes==16); }
    { reset(4,11); DSPDevice d; assert(d.beginIpc4TraceCapture());
      assert(writes==1 && ram[0xc1000/4]==11 && d.traceArchiveBytes==56);
      for(unsigned i=0;i<7;i++) assert(d.traceArchive[48+i]==i+4);
      d.drainIpc4Trace(); assert(writes==1); }
    { reset(4086,3); DSPDevice d; assert(d.beginIpc4TraceCapture());
      const UInt8 expect[]={4086%251,4087%251,0,1,2};
      assert(!std::memcmp(d.traceArchive+48,expect,5)); assert(ram[0xc1000/4]==3); }
    { reset(0,4); mutate=1; DSPDevice d; assert(d.beginIpc4TraceCapture());
      assert(ram[0xc1000/4]==4); d.setIpc4TracePhase(3);
      assert(ram[0xc1000/4]==8 && d.traceSequence==2); }
    { reset(0,4087); DSPDevice d; assert(d.beginIpc4TraceCapture());
      assert(flags(d)==1 && ((UInt32*)d.traceArchive)[3]==1); }
    { reset(0,4088); DSPDevice d; assert(d.beginIpc4TraceCapture());
      assert(flags(d)==2 && writes==0 && d.traceArchiveStopped); }
    for(unsigned mode=2;mode<=3;mode++) {
      reset(0,4); mutate=mode; DSPDevice d; assert(d.beginIpc4TraceCapture());
      assert(flags(d)==2 && writes==0 && d.traceArchiveBytes==16);
    }
    { reset(); DSPDevice d; assert(d.beginIpc4TraceCapture());
      d.traceArchiveBytes=d.TRACE_ARCHIVE_CAPACITY-32; ram[0xc1004/4]=4;
      d.drainIpc4Trace(); assert(flags(d)==4 && writes==0 && ram[0xc1000/4]==0); }
    { reset(); DSPDevice d; d.len4=0xcffff; assert(!d.beginIpc4TraceCapture() && writes==0); }
    { reset(); DSPDevice d; d.dry=true; assert(!d.beginIpc4TraceCapture() && writes==0); }
    { reset(0,4); ram[0xc0004/4]=0x54524300; DSPDevice d;
      assert(d.beginIpc4TraceCapture()); assert(writes==0 && d.traceArchiveBytes==16); }
    puts("12 mtrace SRAM-consumption scenarios passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ohmydsp-mtrace-test-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness + methods + cases)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', '-fsanitize=address,undefined',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
