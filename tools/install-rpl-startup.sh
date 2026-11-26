#!/bin/sh
# Add/remove only a userspace startup loader. No EFI, kext rebuild or audio restart.
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 022
[ "$(id -u)" = 0 ] || { echo 'run with sudo' >&2; exit 2; }
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE='/Library/Application Support/OhMyDSP'
DEST="$BASE/startup"
PLIST=/Library/LaunchDaemons/com.ohmydsp.startup.plist
LABEL=system/com.ohmydsp.startup
restore() {
    launchctl bootout "$LABEL" 2>/dev/null || true
    rm -rf "$DEST"
    rm -f "$PLIST"
    [ ! -d "$BACKUP/startup" ] || cp -Rp "$BACKUP/startup" "$DEST"
    [ ! -f "$BACKUP/startup.plist" ] || cp -p "$BACKUP/startup.plist" "$PLIST"
    if [ -f "$BACKUP/registered" ] && [ -f "$PLIST" ]; then launchctl bootstrap system "$PLIST"; fi
    echo "Restored startup configuration from $BACKUP (loaded kext left intact)."
}
case "${1:-install}" in
rollback)
    BACKUP=${2:?supply the backup path printed at installation}
    case "$BACKUP" in "$BASE"/backups/startup.*) ;; *) echo 'invalid backup path' >&2; exit 2;; esac
    [ -f "$BACKUP/complete" ] || { echo 'incomplete backup' >&2; exit 1; }
    restore; exit;;
install) ;;
*) echo 'usage: install-rpl-startup.sh [install | rollback BACKUP]' >&2; exit 2;;
esac
shasum -a 256 -c "$ROOT/tools/rpl-startup-pins.sha256"
kmutil showloaded --bundle-identifier com.ohmydsp.OhMyDSP.IPC4Session | grep -F '2C2DF9E1-4F38-32B3-B31B-54D151936299' >/dev/null || { echo 'load and verify the qualified session first' >&2; exit 1; }
ioreg -rc OhMyDSP -l -w0 | grep -q '"ipc4-session-state" = "stopped"' || { echo 'close recording apps before installing startup loading' >&2; exit 1; }
mkdir -p "$BASE/backups" /Library/LaunchDaemons
BACKUP=$(mktemp -d "$BASE/backups/startup.XXXXXX")
[ ! -d "$DEST" ] || cp -Rp "$DEST" "$BACKUP/startup"
[ ! -f "$PLIST" ] || cp -p "$PLIST" "$BACKUP/startup.plist"
if launchctl print "$LABEL" >/dev/null 2>&1; then touch "$BACKUP/registered"; fi
touch "$BACKUP/complete"
echo "Rollback backup: $BACKUP"
trap 'code=$?; if [ "$code" -ne 0 ]; then restore; fi' EXIT
launchctl bootout "$LABEL" 2>/dev/null || true
rm -rf "$DEST"; mkdir "$DEST"
cp "$ROOT/tools/rpl-startup-loader.sh" "$DEST/load-session.sh"
cp "$ROOT/tools/rpl-startup-pins.sha256" "$DEST/pins.sha256"
chown -R root:wheel "$DEST"; chmod 755 "$DEST" "$DEST/load-session.sh"; chmod 644 "$DEST/pins.sha256"
"$DEST/load-session.sh" check
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>com.ohmydsp.startup</string>
<key>ProgramArguments</key><array><string>$DEST/load-session.sh</string></array>
<key>RunAtLoad</key><true/>
<key>KeepAlive</key><false/>
<key>StandardOutPath</key><string>/Library/Logs/OhMyDSP/startup-launch.log</string>
<key>StandardErrorPath</key><string>/Library/Logs/OhMyDSP/startup-launch.log</string>
</dict></plist>
PL
mkdir -p /Library/Logs/OhMyDSP
chown root:wheel "$PLIST"; chmod 644 "$PLIST"
plutil -lint "$PLIST"
launchctl bootstrap system "$PLIST"
trap - EXIT
echo 'Startup loader registered; reboot qualification is still required.'
echo "Rollback: sudo $0 rollback \"$BACKUP\""
