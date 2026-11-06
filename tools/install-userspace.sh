#!/bin/sh
# Phase 6.1c: install / remove the userspace half (root daemon + AudioServerPlugIn). Build first: make -C ohmydsp/userspace
# Usage: sudo sh tools/install-userspace.sh            install (or update) and restart coreaudiod
#        sudo sh tools/install-userspace.sh uninstall   remove everything again
# Optional env: FIRMWARE_DIR (+ FIRMWARE_SHA256) = folder with sof-cnl.ri and LICENCE.Intel to install; the pane passes the firmware it downloaded.
# Needs no kext approval and no reboot. coreaudiod restarts for a moment (audio glitches, no data loss).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; B="${PAYLOAD:-$ROOT/ohmydsp/userspace/build}"   # PAYLOAD: directory with ohmydspd + OhMyDSP.driver (the pane passes its Resources)
DDIR="/Library/Application Support/OhMyDSP"; DST="$DDIR/ohmydspd"
PLIST=/Library/LaunchDaemons/com.ohmydsp.daemon.plist; DRV=/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver
launchctl bootout system/com.ohmydsp.daemon 2>/dev/null || true
if [ "$1" = "uninstall" ]; then
    rm -rf "$DRV" "$PLIST" "$DDIR"; killall coreaudiod 2>/dev/null || true; echo "removed"; exit 0
fi
[ -x "$B/ohmydspd" ] && [ -d "$B/OhMyDSP.driver" ] || { echo "build first: make -C ohmydsp/userspace"; exit 1; }
mkdir -p "$DDIR"
if [ -n "$FIRMWARE_DIR" ]; then
    cp "$FIRMWARE_DIR/sof-cnl.ri" "$FIRMWARE_DIR/LICENCE.Intel" "$DDIR/"
    [ "$(shasum -a 256 "$DDIR/sof-cnl.ri" | cut -d' ' -f1)" = "$FIRMWARE_SHA256" ] || { rm -f "$DDIR/sof-cnl.ri"; echo "firmware checksum mismatch"; exit 1; }
fi
[ -f "$B/dmic-p10.ipc" ] && cp "$B/dmic-p10.ipc" "$DDIR/"
cp "$B/ohmydspd" "$DST"; chown -R root:wheel "$DDIR"; chmod 755 "$DST"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.ohmydsp.daemon</string>
<key>ProgramArguments</key><array><string>$DST</string></array>
<key>MachServices</key><dict><key>com.ohmydsp.daemon</key><true/></dict>
<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
</dict></plist>
PL
chown root:wheel "$PLIST"; chmod 644 "$PLIST"
mkdir -p "$(dirname "$DRV")"; rm -rf "$DRV"; cp -R "$B/OhMyDSP.driver" "$DRV"; chown -R root:wheel "$DRV"; chmod -R go-w "$DRV"
launchctl bootstrap system "$PLIST"
killall coreaudiod 2>/dev/null || true
echo "installed; coreaudiod restarting. Check: system_profiler SPAudioDataType | grep -A4 'Internal Microphone'"
