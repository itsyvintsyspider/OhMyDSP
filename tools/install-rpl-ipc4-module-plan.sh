#!/bin/sh
# Generate and install the recovered NHLT-backed IPC4 replay blob.
# This only writes an offline plan and a root-owned asset. It does not load a
# kext, power the DSP, send IPC, or touch AppleHDA.
set -eu

# Keep earlier module/bind experiments unchanged unless explicitly selected.
case "${1:-}" in
    "") ;;
    --tdfb-config|--eq-config) [ "$#" -eq 1 ] || exit 2 ;;
    *) echo "usage: $0 [--tdfb-config|--eq-config]" >&2; exit 2 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "run as administrator: sudo $0" >&2
    exit 2
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NHLT=${NHLT:-"$ROOT/NHLT.bin"}
FIRMWARE=${FIRMWARE:-"/Library/Application Support/OhMyDSP/rpl/sof-rpl.ri"}
TOPOLOGY=${TOPOLOGY:-"/Library/Application Support/OhMyDSP/rpl/sof-hda-generic-cavs25-2ch.tplg"}
DEST_DIR="/Library/Application Support/OhMyDSP/rpl"
DEST="$DEST_DIR/rpl-dmic.i4pc"

[ -f "$NHLT" ] || { echo "missing NHLT table: $NHLT" >&2; exit 1; }
mkdir -p "$DEST_DIR"
WORK=$(mktemp -d /private/tmp/ohmydsp-rpl-plan.XXXXXX)
PLAN="$WORK/messages.json"
BLOB=${BLOB:-"$WORK/vendor-blob.bin"}
STAGED=$(mktemp "$DEST.stage.XXXXXX") || { rmdir "$WORK"; exit 1; }
trap 'rc=$?; rm -f "$STAGED"; if [ "$rc" -ne 0 ]; then rm -rf "$WORK"; fi' EXIT
trap 'exit 1' HUP INT TERM
if [ -f "$DEST" ]; then
    BACKUP=$(mktemp "$DEST.backup.XXXXXX")
    cp -p "$DEST" "$BACKUP"
    echo "previous asset preserved at: $BACKUP"
fi
python3 "$ROOT/tools/extract-nhlt-vendor-blob.py" "$NHLT" "$BLOB" \
    --report "$WORK/endpoints.json" >"$WORK/extract.json"
python3 "$ROOT/tools/gen-ipc4-messages.py" \
    --firmware "$FIRMWARE" \
    --topology "$TOPOLOGY" \
    --nhlt-vendor-blob "$BLOB" \
    --output "$PLAN" \
    --binary-output "$STAGED" "$@" >"$WORK/generator.json"

test -s "$STAGED"
chown root:wheel "$STAGED"
chmod 0644 "$STAGED"
mv "$STAGED" "$DEST"

echo "installed NHLT-backed IPC4 module-plan asset: $DEST"
echo "reviewable plan: $PLAN"
shasum -a 256 "$DEST"
