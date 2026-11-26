#!/bin/sh
# Stage and load the independent RPL IPC4 capture-DMA probe.
# This variant patches only the host-copier stream index, uses SD5 for a
# bounded 1000 ms PCM probe, and never registers an IOAudio device.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "run as administrator: sudo $0" >&2
    exit 2
fi

ID=com.ohmydsp.OhMyDSP.IPC4CaptureDMAProbe
EXPERIMENT_IDS='com.ohmydsp.OhMyDSP com.ohmydsp.OhMyDSP.IPC4Probe com.ohmydsp.OhMyDSP.IPC4ModuleProbe com.ohmydsp.OhMyDSP.IPC4BindProbe com.ohmydsp.OhMyDSP.IPC4CaptureProbe com.ohmydsp.OhMyDSP.IPC4CaptureDMAProbe'
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/ohmydsp/build/capture-dma-manual-live-rpl-core-standalone/OhMyDSP.kext"
DST="/Library/Application Support/dmic-macos/kext/rpl-manual-ipc4-capture-dma-probe/OhMyDSP.kext"
ASSET="/Library/Application Support/OhMyDSP/rpl/rpl-dmic.i4pc"

for loaded_id in $EXPERIMENT_IDS; do
    if kmutil showloaded --bundle-identifier "$loaded_id" 2>/dev/null | grep -q "$loaded_id"; then
        echo "an OhMyDSP experiment ($loaded_id) is already loaded. Reboot or unload it first." >&2
        exit 1
    fi
done
[ -f "$SRC/Contents/Info.plist" ] || { echo "missing IPC4 capture-DMA artifact: $SRC" >&2; exit 1; }
[ -f "$ASSET" ] || { echo "missing NHLT-backed IPC4 asset: $ASSET" >&2; exit 1; }
# Catch a missing run-12 configuration before staging a kext or asking macOS
# to approve it. The kernel repeats this check before graph IPC.
python3 - "$ASSET" <<'PY'
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
try:
    magic, count = struct.unpack_from('<4sI', data)
    assert magic == b'I4PC' and count == 17
    pos = 8
    for i in range(count):
        length, extension, primary = struct.unpack_from('<III', data, pos)
        assert length >= 12 and length % 4 == 0 and pos + length <= len(data)
        if i == 15:
            assert (length, extension, primary) == (800, 0x30000314, 0x44000014)
            assert struct.unpack_from('<I', data, pos + 12)[0] == 788
        if i == 16:
            assert (length, extension, primary) == (100, 0x30000058, 0x4400000a)
            assert struct.unpack_from('<3I', data, pos + 12) == (88, 2, 1)
        pos += length
    assert pos == len(data)
except (AssertionError, struct.error):
    sys.exit('run-12 requires EQ and TDFB coefficients: run sudo ./tools/install-rpl-ipc4-module-plan.sh --eq-config first')
print('validated capture asset: 15 graph messages + TDFB and EQ coefficients')
PY
/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:OhMyDSP:IOPCIMatch' "$SRC/Contents/Info.plist" | grep -qi '0x51ca8086' || {
    echo "refusing artifact without Raptor Lake match" >&2
    exit 1
}

mkdir -p "$(dirname -- "$DST")"
rm -rf "$DST"
cp -R "$SRC" "$DST"
chown -R root:wheel "$DST"
chmod -R go-w "$DST"
kmutil load -p "$DST"
kmutil showloaded --bundle-identifier "$ID"
echo "loaded manual RPL IPC4 capture-DMA probe"
echo "run-13 records about 1 second of PCM after the probe delay (default 30 seconds)."
echo "With the default delay, speak continuously about 20-40 seconds after loading; then collect diagnostics."
