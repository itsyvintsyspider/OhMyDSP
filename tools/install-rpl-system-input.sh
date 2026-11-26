#!/bin/sh
# Install only the OhMyDSP daemon/HAL. Never alters EFI, firmware or audio defaults.
set -eu
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 2; }
# System components must remain readable by CoreAudio even when the caller protects logs with umask 077.
umask 022
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE='/Library/Application Support/OhMyDSP'
DAEMON="$BASE/ohmydspd"
PLIST=/Library/LaunchDaemons/com.ohmydsp.daemon.plist
DRIVER=/Library/Audio/Plug-Ins/HAL/OhMyDSP.driver
BUILD="$ROOT/ohmydsp/userspace/build"
restore() {
    launchctl bootout system/com.ohmydsp.daemon 2>/dev/null || true
    rm -rf "$DRIVER"
    rm -f "$DAEMON" "$PLIST"
    [ ! -e "$BACKUP/OhMyDSP.driver" ] || cp -Rp "$BACKUP/OhMyDSP.driver" "$DRIVER"
    [ ! -e "$BACKUP/ohmydspd" ] || cp -p "$BACKUP/ohmydspd" "$DAEMON"
    [ ! -e "$BACKUP/daemon.plist" ] || cp -p "$BACKUP/daemon.plist" "$PLIST"
    if [ -e "$BACKUP/was-running" ] && [ -f "$PLIST" ]; then launchctl bootstrap system "$PLIST"; fi
    killall coreaudiod 2>/dev/null || true
    echo "restored userspace components from $BACKUP"
}
if [ "${1:-install}" = rollback ]; then
    BACKUP=${2:?provide the backup directory printed at installation}
    case "$BACKUP" in "$BASE"/backups/system-input.*) ;; *) echo 'invalid backup location' >&2; exit 2;; esac
    [ -f "$BACKUP/complete" ] || { echo 'incomplete backup' >&2; exit 1; }
    restore; exit
fi
[ "${1:-install}" = install ] || exit 2
kmutil showloaded --bundle-identifier com.ohmydsp.OhMyDSP.IPC4Session 2>/dev/null | grep -q com.ohmydsp.OhMyDSP.IPC4Session || { echo 'load the IPC4Session kext first' >&2; exit 1; }
IOREG=$(ioreg -rc OhMyDSP -l -w0)
printf '%s\n' "$IOREG" | grep -q '"ipc4-session-ready" = Yes' || { echo 'session not ready' >&2; exit 1; }
# On a freshly prepared kext the first START has not yet published a session state.
if ! printf '%s\n' "$IOREG" | grep -q '"ipc4-session-state" = "stopped"'; then
    if printf '%s\n' "$IOREG" | grep -Eq '"ipc4-session-(state|number)" =|IPC4 session [0-9]+:' ||
       ! printf '%s\n' "$IOREG" | grep -Fq 'live capture ready: DSP idle, powers up when the user client starts the stream'; then
        echo 'close recording apps before updating userspace; session must be prepared or stopped' >&2
        exit 1
    fi
fi
[ -x "$BUILD/ohmydspd" ] && [ -d "$BUILD/OhMyDSP.driver" ] || { echo 'build userspace first' >&2; exit 1; }
codesign --verify --strict "$BUILD/ohmydspd"
codesign --verify --deep --strict "$BUILD/OhMyDSP.driver"
mkdir -p "$BASE/backups" /Library/Audio/Plug-Ins/HAL /Library/LaunchDaemons
BACKUP=$(mktemp -d "$BASE/backups/system-input.XXXXXX")
[ ! -e "$DRIVER" ] || cp -Rp "$DRIVER" "$BACKUP/OhMyDSP.driver"
[ ! -e "$DAEMON" ] || cp -p "$DAEMON" "$BACKUP/ohmydspd"
[ ! -e "$PLIST" ] || cp -p "$PLIST" "$BACKUP/daemon.plist"
if launchctl print system/com.ohmydsp.daemon >/dev/null 2>&1; then touch "$BACKUP/was-running"; fi
touch "$BACKUP/complete"
echo "rollback backup: $BACKUP"
# Restore the exact previous components if any installation step fails.
trap 'result=$?; if [ "$result" -ne 0 ]; then restore; fi' EXIT
launchctl bootout system/com.ohmydsp.daemon 2>/dev/null || true
cp "$BUILD/ohmydspd" "$DAEMON"
chown root:wheel "$DAEMON"; chmod 755 "$DAEMON"
rm -rf "$DRIVER"; cp -R "$BUILD/OhMyDSP.driver" "$DRIVER"
chown -R root:wheel "$DRIVER"; chmod -R a+rX,go-w "$DRIVER"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.ohmydsp.daemon</string>
<key>ProgramArguments</key><array><string>$DAEMON</string></array>
<key>MachServices</key><dict><key>com.ohmydsp.daemon</key><true/></dict>
<key>RunAtLoad</key><true/><key>KeepAlive</key><true/>
</dict></plist>
PL
chown root:wheel "$PLIST"; chmod 644 "$PLIST"
plutil -lint "$PLIST"
launchctl bootstrap system "$PLIST"
killall coreaudiod 2>/dev/null || true
trap - EXIT
echo 'Installed Internal Microphone (DMIC); CoreAudio is restarting (brief audio interruption).'
echo "Rollback: sudo $0 rollback \"$BACKUP\""
