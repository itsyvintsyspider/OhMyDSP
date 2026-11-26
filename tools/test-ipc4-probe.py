#!/usr/bin/env python3
"""Run both kernel blob parsers and the probe cleanup sequence without a device."""
import re
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'ohmydsp/src/DSPDevice.cpp').read_text()
# Extract the named definitions through their top-level closing braces.
pattern = r'^(?:bool|void) DSPDevice::(?:loadPipelineBlob|captureDescriptorIdle|finishIpc4CaptureProbe)\(.*?^\}$'
methods = '\n'.join(re.findall(pattern, source, re.MULTILINE | re.DOTALL)) + '\n'
harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
using UInt32=uint32_t; using UInt8=uint8_t;
#define LOG(...) ((void)0)
const char *kBlobPath="fixture";
std::vector<std::string> calls;
std::string failure;
bool op(const char *step){calls.push_back(step);return failure!=step;}
struct Blob {
    std::vector<UInt32> words;
    void *getBytesNoCopy(){return words.data();}
    unsigned getLength(){return words.size()*4;}
    void release(){}
} blob;
enum {IPC4_MAX_PAYLOAD=4096, IPC4_STATE_PAUSED=3, IPC4_STATE_RESET=2,
      SD_CTL_RUN=2,SD_CTL_SRST=1,SD_TAG_SHIFT=20, SD_CBL=8, SD_BDLPL=24, SD_BDLPU=28};
unsigned residualOffset=0, residualValue=0;
UInt32 r32(volatile UInt8*,unsigned offset){return offset==residualOffset?residualValue:0;}
struct DSPDevice {
    Blob *ipcBlob=nullptr; UInt32 ipcCount=0,ipcOff[64]={};
    bool ipc4Blob=false,streamTouchedHw=true;
    UInt8 *b0=nullptr; UInt32 sd=0,ppBase=0,sdBit=1;
    bool loadFile(const char*,unsigned,Blob**out){*out=&blob;return true;}
    struct StopInfo {unsigned stsEnd=0;};
    bool stopDma(StopInfo*s){if(failure=="dma-error")s->stsEnd=8;return op("stop");}
    bool sendIpc4PipelineStateMulti(unsigned state,unsigned,unsigned,const char*){
        return op(state==IPC4_STATE_PAUSED?"pause":"reset");
    }
    void setIpc4TracePhase(unsigned){}
    bool setIpc4Trace(bool on){assert(!on);return op("disable-trace");}
    bool freeStream(){return op("free");}
    void teardown(bool){op("teardown");}
    bool checkIdle(){return op("idle");}
    bool captureDescriptorIdle();
    bool loadPipelineBlob(); bool finishIpc4CaptureProbe(bool,bool);
};
'''
cases = r'''
int main() {
    DSPDevice d;
    const std::vector<UInt32> ipc3={0x43504944,1,12,0,12,1,0};
    const std::vector<UInt32> ipc4={0x43503449,1,12,0,1};
#ifdef OHMYDSP_RPL_LIVE
    auto correct=ipc4,wrong=ipc3;
#else
    auto correct=ipc3,wrong=ipc4;
#endif
    blob.words=correct;assert(d.loadPipelineBlob() && d.ipcCount==1);
    blob.words=wrong;assert(!d.loadPipelineBlob());
    correct.pop_back();blob.words=correct;assert(!d.loadPipelineBlob());
    assert(calls.empty());
    const std::vector<std::string> expected={"pause","stop","reset","disable-trace","free","teardown","idle"};
    for(auto f:{"","pause","stop","dma-error","reset","disable-trace","free","idle"}) {
        failure=f;calls.clear();assert(d.finishIpc4CaptureProbe(true,true)==failure.empty());
        assert(calls==expected);
    }
    failure="";
    for(auto reg:std::vector<std::pair<unsigned,unsigned>>{{0,SD_CTL_RUN},{0,SD_CTL_SRST},{0,1u<<SD_TAG_SHIFT},
            {SD_CBL,4},{SD_BDLPL,128},{SD_BDLPU,1},{4,d.sdBit}}) {
        residualOffset=reg.first; residualValue=reg.second;
        calls.clear();assert(!d.finishIpc4CaptureProbe(true,true));assert(calls==expected);
    }
    residualValue=0;
    calls.clear(); failure=""; assert(d.finishIpc4CaptureProbe(false,false));
    assert(calls==std::vector<std::string>({"stop","free","teardown","idle"}));
}
'''
# Run the manual probe's cleanup and publication tail for both observation outcomes.
probe = (root / 'ohmydsp/src/OhMyDSP.cpp').read_text()
start = probe.index('        if (ok && ipc4Prepared) ok = rpl.observeIpc4CaptureStream(')
tail = probe[start:probe.index('#endif', start)]
publication = r'''
#include <cassert>
#include <string>
#include <vector>
using Bytes = std::vector<unsigned char>;
Bytes property;
void setProperty(const char* key,const unsigned char* data,unsigned size) {
    assert(std::string(key)=="ipc4-mtrace-stream");
    property.assign(data,data+size);
}
struct Probe {
    bool observed,clean;
    Bytes archive;
    bool observeIpc4CaptureStream(unsigned){return observed;}
    bool finishIpc4CaptureProbe(bool,bool){archive.push_back(6);return clean;}
    const unsigned char* ipc4TraceData(){return archive.data();}
    unsigned ipc4TraceSize(){return archive.size();}
};
bool run(bool observed,bool cleanup) {
    Probe rpl{observed,cleanup,{1,2,3}};
    bool ok=true,ipc4Prepared=true,ipc4Graph=true,ipc4Trace=true;
''' + tail + r'''
    return ok;
}
int main() {
    for(bool observation:{false,true}) for(bool cleanup:{false,true}) {
        property.clear();
        assert(run(observation,cleanup)==(observation && cleanup));
        assert(property==Bytes({1,2,3,6}));
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp)/'probe.cpp'
    test.write_text(harness + methods + cases)
    for flags in ([], ['-DOHMYDSP_RPL_LIVE']):
        exe = Path(tmp)/'test'
        subprocess.run(['xcrun','clang++','-std=c++17',*flags,str(test),'-o',str(exe)],check=True)
        subprocess.run([exe],check=True)
    test.write_text(publication)
    subprocess.run(['xcrun','clang++','-std=c++17',str(test),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('IPC3/IPC4 formats, truncation and probe cleanup failure matrix PASS')
