#!/usr/bin/env python3
"""Compile the actual IPC4 transport methods against a scripted MMIO device."""
import subprocess
import tempfile
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'ohmydsp/src/DSPDevice.cpp').read_text()
# Extract the named definitions through their top-level closing braces.
pattern = r'^(?:bool|void) DSPDevice::(?:ackIpc4Notification|ipc4Tx)\(.*?^\}$'
methods = '\n'.join(re.findall(pattern, source, re.MULTILINE | re.DOTALL)) + '\n'
harness = r'''
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <deque>
using UInt32 = uint32_t;
using UInt8 = uint8_t;
#define OHMYDSP_RPL_LIVE 1
#define LOG(...) do {} while (0)
enum : UInt32 { HIPCTDR=0xc0, HIPCTDA=0xc4, HIPCTDD=0xc8, HIPCIDR=0xd0,
    HIPCIDA=0xd4, HIPCIDD=0xd8, BIT31=0x80000000, IPC4_MAX_PAYLOAD=4096,
    IPC4_REPLY_DIR=1u<<29, IPC4_REPLY_STATUS=0xffffff };
struct Event { unsigned at; UInt32 primary; };
static std::deque<Event> events, afterSend;
static unsigned ticks, doneAt, ackCount, requiredAcks;
static bool sent, doneAcked;
static void IOSleep(unsigned ms) { ticks += ms; }
static UInt32 r32(volatile UInt8*, UInt32 offset) {
    if (offset==HIPCTDR) return !events.empty() && events.front().at<=ticks ? events.front().primary | BIT31 : 0;
    if (offset==HIPCIDA) return sent && !doneAcked && ticks>=doneAt && ackCount>=requiredAcks ? BIT31 : 0;
    return 0;
}
class DSPDevice {
public:
    volatile UInt8 *b4=nullptr;
    UInt32 downbox=0xa0000, upbox=0x81000, downSize=4096, upSize=4096;
    void W4(UInt32 offset, UInt32, const char*) {
        if (offset==HIPCTDR) { assert(!events.empty()); events.pop_front(); ackCount++; }
        if (offset==HIPCIDR) { sent=true; events.insert(events.end(), afterSend.begin(), afterSend.end()); }
    }
    void U4(UInt32 offset, UInt32, UInt32, const char*) { if(offset==HIPCIDA) doneAcked=true; }
    bool ackIpc4Notification(UInt32, const char*);
    void drainIpc4Trace() {}
    bool ipc4Tx(const UInt32*, UInt32, UInt32, UInt32, const char*, UInt32*, UInt32*);
};
'''
cases = r'''
static void reset() {
    events.clear(); afterSend.clear(); ticks=0; doneAt=0; ackCount=0;
    requiredAcks=0; sent=false; doneAcked=false;
}
static bool tx() {
    DSPDevice device;
    return device.ipc4Tx(nullptr, 0, 0x13000004, 1, "test", nullptr, nullptr);
}
int main() {
    reset(); afterSend={{0,0x33000000}}; assert(tx());
    reset(); events={{0,0x1b060000}}; afterSend={{3,0x33000000}};
    assert(tx() && ackCount==2);
    // A log notification must be ACKed before the simulated firmware accepts.
    reset(); requiredAcks=1; afterSend={{0,0x1b060000},{4,0x33000000}};
    assert(tx() && ackCount==2);
    reset(); afterSend={{1,0x1b060000},{2,0x1b050000},{3,0x33000000}};
    assert(tx() && ackCount==3);
    reset(); afterSend={{0,0x33000007}}; assert(!tx());
    reset(); afterSend={{0,0x34000000}}; assert(!tx()); // wrong opcode
    reset(); afterSend={{0,0x73000000}}; assert(!tx()); // wrong target
    reset(); afterSend={{0,0x13000000}}; assert(!tx()); // request, not reply
    reset(); assert(!tx() && ticks<=1000); // no reply
    reset(); doneAt=1001; assert(!tx() && ticks<=1000); // no DONE
    reset(); for (unsigned n=0;n<40;n++) afterSend.push_back({n,0x1b060000});
    assert(!tx() && ackCount==32 && ticks<=1000);
    reset(); events={{0,0x33000000}}; assert(!tx() && !sent); // stale reply
    puts("12 IPC4 transport scenarios passed");
}
'''
with tempfile.TemporaryDirectory(prefix='ohmydsp-ipc4-test-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness + methods + cases)
    subprocess.run(['xcrun', 'clang++', '-std=c++14', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
