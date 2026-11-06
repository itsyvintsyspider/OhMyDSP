#!/bin/sh
# Stage the freshly built STANDALONE OhMyDSP.kext (make -C ohmydsp LILU=0; no Lilu dependency) root-owned and load it manually with kmutil (CLAUDE.md: manual loads first).
# Needed since Phase 5: the kext depends on IOAudioFamily, which lives in the System KC, so OpenCore cannot inject it into the Boot KC.
# Usage: sudo sh tools/load-kext.sh        (unloads a previously loaded copy first)   |   sudo sh tools/load-kext.sh unload
#        sudo sh tools/load-kext.sh core   Phase 6 core kext (make -C ohmydsp CORE=1 LILU=0): no IOAudioFamily, user client for tools/coretest.c
set -e
ID=com.ohmydsp.OhMyDSP
V=standalone; [ "$1" = "core" ] && V=core-standalone
SRC="$(cd "$(dirname "$0")/.." && pwd)/ohmydsp/build/$V/OhMyDSP.kext"
DST="/Library/Application Support/dmic-macos/kext/OhMyDSP.kext"
launchctl bootout system/com.ohmydsp.daemon 2>/dev/null || true   # the daemon holds the kext user client open: unloading is impossible while it runs
if kmutil showloaded --bundle-identifier "$ID" 2>/dev/null | grep -q "$ID"; then kmutil unload -b "$ID"; fi
[ "$1" = "unload" ] && exit 0
rm -rf "$DST"; mkdir -p "$(dirname "$DST")"; cp -R "$SRC" "$DST"
chown -R root:wheel "$DST"; chmod -R go-w "$DST"
kmutil load -p "$DST"
kmutil showloaded --bundle-identifier "$ID" | grep "$ID" || echo "NOT LOADED"
