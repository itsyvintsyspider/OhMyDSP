#!/bin/sh
# Install validated Raptor Lake IPC4 assets only. This script does not load a
# kext, start firmware, or access hardware.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "run as administrator: sudo $0" >&2
    exit 2
fi

VERSION=2026.09.2
ARCHIVE_SHA256=5f7bc30b2b5c1e6b0da55a33fc44e2b7f56ae9b06952edcde614f51ef6b943e1
FIRMWARE_SHA256=351325786e53ca7cd23ffc1462d82913f3e0c08ad78f89053d85e4b6703cf12b
FIRMWARE_SIZE=707036
ROOT="/Library/Application Support/OhMyDSP/rpl"
TMP="$(mktemp -d /tmp/ohmydsp-rpl-assets.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT INT TERM

ARCHIVE="$TMP/sof-bin-$VERSION.tar.gz"
curl -Lks --fail --max-time 120 "https://github.com/thesofproject/sof-bin/releases/download/v$VERSION/sof-bin-$VERSION.tar.gz" -o "$ARCHIVE"
ACTUAL_ARCHIVE_SHA256=$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')
[ "$ACTUAL_ARCHIVE_SHA256" = "$ARCHIVE_SHA256" ] || { echo "archive checksum mismatch" >&2; exit 1; }

mkdir "$TMP/tree"
tar -xzf "$ARCHIVE" -C "$TMP/tree" --strip-components=1
FIRMWARE="$TMP/tree/sof-ipc4/rpl/intel-signed/sof-rpl.ri"
TOPOLOGY="$TMP/tree/sof-ipc4-tplg/sof-hda-generic-cavs25-2ch.tplg"
LICENSE="$TMP/tree/LICENCE.Intel"
[ -f "$FIRMWARE" ] && [ -f "$TOPOLOGY" ] && [ -f "$LICENSE" ] || { echo "required RPL assets missing" >&2; exit 1; }
[ "$(wc -c < "$FIRMWARE" | tr -d ' ')" = "$FIRMWARE_SIZE" ] || { echo "firmware size mismatch" >&2; exit 1; }
[ "$(shasum -a 256 "$FIRMWARE" | awk '{print $1}')" = "$FIRMWARE_SHA256" ] || { echo "firmware checksum mismatch" >&2; exit 1; }

mkdir -p "$ROOT"
cp "$FIRMWARE" "$ROOT/sof-rpl.ri"
cp "$TOPOLOGY" "$ROOT/sof-hda-generic-cavs25-2ch.tplg"
cp "$LICENSE" "$ROOT/LICENCE.Intel"
chown -R root:wheel "$ROOT"
chmod -R go-w "$ROOT"
echo "installed validated RPL IPC4 assets under $ROOT"
