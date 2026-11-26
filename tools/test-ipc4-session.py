#!/usr/bin/env python3
"""Execute the actual DSP session orchestration against fault-injected hardware stubs."""
import re
from pathlib import Path
import subprocess,tempfile
source=(Path(__file__).resolve().parents[1]/'ohmydsp/src/DSPDevice.cpp').read_text()
# Extract the named definitions through their top-level closing braces.
pattern = r'^(?:bool|void) DSPDevice::(?:startIpc4Session|stopIpc4Session)\(.*?^\}$'
methods = '\n'.join(re.findall(pattern, source, re.MULTILINE | re.DOTALL)) + '\n'
prefix=r'''
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
using UInt32=uint32_t; using UInt8=uint8_t;
#define LOG(...) do{}while(0)
static std::vector<std::string> calls;
static std::string fail;
static bool op(const char *s){calls.push_back(s);return fail!=s;}
static UInt32 r32(volatile UInt8*,UInt32){return fail=="poweroff"?0:0xf0f;}
static void IOFree(void*p,unsigned){free(p);}
enum { ADSPCS=4 };
class DSPDevice {
public:
 enum { IPC4_STATE_PAUSED=3, IPC4_STATE_RUNNING=4, IPC4_STATE_RESET=2, TRACE_ARCHIVE_CAPACITY=65536 };
 bool dry=false,sessionBlocked=false,sessionTouched=false,stuck=false,persistent=true,sessionGraph=false,sessionTrace=false;
 bool streamTouchedHw=false;
 bool captureDescriptorIdle(){return fail!="descriptor";}
 int sdIdx=-1; unsigned capBytes=64;
 UInt8 *traceArchive=nullptr;unsigned traceArchiveBytes=0,traceSequence=0,tracePhase=0;bool traceArchiveStopped=false;
 volatile UInt8 *b4=nullptr;
 struct Buffer { UInt8 bytes[64];void*getBytesNoCopy(){return bytes;} } buffer;
 struct { Buffer*cap; }pb{&buffer};
 struct StopInfo {};
 ~DSPDevice(){free(traceArchive);}
 bool checkIdle(){return op("idle");}
 bool powerUp(bool){return op("power");}
 bool beginIpc4TraceCapture(){traceArchive=(UInt8*)malloc(65536);return op("trace");}
 void setIpc4TracePhase(unsigned phase){tracePhase=phase;}
 bool sendIpc4CaptureGraph(unsigned sd){assert(sd==5);return op("graph");}
 bool sendIpc4PipelineStateMulti(unsigned s,unsigned a,unsigned b,const char *what){
   if(s==IPC4_STATE_PAUSED)assert(a==0&&b==1);else assert(a==1&&b==0);
   return op(what);
 }
 bool setupIpc4CaptureStream(unsigned sd){streamTouchedHw=true;sdIdx=sd;return op("setup");}
 bool setIpc4Trace(bool on){return op(on?"enable":"disable");}
 bool startIpc4CaptureDma(){return op("dma");}
 bool stopDma(StopInfo*){return op("stop");}
 bool freeStream(){streamTouchedHw=false;sdIdx=-1;return op("free");}
 void teardown(bool){op("teardown");}
 bool startIpc4Session();bool stopIpc4Session();
};
'''
suffix=r'''
int main(){
 {DSPDevice d;assert(d.startIpc4Session());assert(d.stopIpc4Session());
  auto first=calls; calls.clear();assert(d.startIpc4Session());assert(d.stopIpc4Session());assert(first==calls);
  calls.clear();assert(d.stopIpc4Session());assert(calls.empty());}
 {DSPDevice d;fail="";assert(d.startIpc4Session());assert(d.stopIpc4Session());
  assert(d.traceArchive);fail="idle";assert(!d.startIpc4Session());assert(!d.traceArchive);}
 const char *startFailures[]={"idle","power","trace","graph","IPC4 session PAUSED","setup","enable","IPC4 session RUNNING","dma"};
 for(auto f:startFailures){DSPDevice d;calls.clear();fail=f;assert(!d.startIpc4Session());
   assert(!d.sessionTouched && d.sdIdx==-1);if(fail!="idle")assert(calls.back()=="teardown");}
 const char *stopFailures[]={"IPC4 session PAUSE cleanup","stop","IPC4 session RESET cleanup","disable","free","descriptor","poweroff"};
 for(auto f:stopFailures){DSPDevice d;fail="";assert(d.startIpc4Session());calls.clear();fail=f;
   assert(!d.stopIpc4Session());assert(d.sessionBlocked);assert(calls.back()=="teardown");
   calls.clear();fail="";assert(!d.startIpc4Session());assert(calls.empty());assert(!d.traceArchive);}
 puts("IPC4 session: repeat/idempotence, 9 start failures, 7 cleanup failures passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ipc4-session-test-') as d:
 p=Path(d);(p/'test.cpp').write_text(prefix+methods+suffix)
 subprocess.run(['xcrun','clang++','-std=c++14','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
