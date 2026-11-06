"""Mic A experiment helper (root). Patches ONE field of pdm[0] in the DAI_CONFIG message of the installed pipeline blob.
usage: sudo python3 -I tools/mica-exp.py show | reset | clk_edge=N | skew=N | polarity_a=N | enable_mic_a=N ...
The kext reads the blob when it starts (reload it: tools/load-kext.sh unload, then load). Pristine copy: local/dmic-p10.ipc.orig"""
import struct, sys, os, shutil
LIVE = "/Library/Application Support/dmic-macos/dmic-p10.ipc"
ORIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "local", "dmic-p10.ipc.orig")
FIELDS = {"id": 0, "enable_mic_a": 1, "enable_mic_b": 2, "polarity_a": 3, "polarity_b": 4, "clk_edge": 5, "skew": 6}   # u16 index inside pdm[0] after a 4-byte reserved word
def locate(b):
    assert b[:4] == b"DIPC"
    n = struct.unpack_from("<I", b, 4)[0]; o = 8
    for _ in range(n):
        ln = struct.unpack_from("<I", b, o)[0]; m = o + 8
        sz, cmd = struct.unpack_from("<II", b, m)
        if cmd == 0x80010000: assert sz == 216; return m + 120 + 4       # pdm[0] u16 fields
        o = m + ((ln + 3) & ~3)
    raise SystemExit("DAI_CONFIG not found")
def show(b):
    p = locate(b); print({k: struct.unpack_from("<H", b, p + 2 * i)[0] for k, i in FIELDS.items()})
if not os.path.exists(ORIG): shutil.copy(LIVE, ORIG); print("saved pristine copy", ORIG)
b = bytearray(open(ORIG, "rb").read()); arg = sys.argv[1] if len(sys.argv) > 1 else "show"
if arg == "show": show(bytearray(open(LIVE, "rb").read()))
elif arg == "reset": open(LIVE, "wb").write(b); print("restored"); show(b)
else:
    p = locate(b)
    for a in sys.argv[1:]:                      # several fields allowed: field=value field=value (all applied to the pristine blob)
        k, v = a.split("="); struct.pack_into("<H", b, p + 2 * FIELDS[k], int(v))
    open(LIVE, "wb").write(b); print("patched", " ".join(sys.argv[1:])); show(b)
