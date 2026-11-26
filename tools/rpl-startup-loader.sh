#!/bin/sh
# One attempt per boot, pinned to the qualified RPL session build. No recording.
set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 022
BASE='/Library/Application Support/OhMyDSP/startup'
KEXT='/Library/Application Support/dmic-macos/kext/rpl-manual-ipc4-session/OhMyDSP.kext'
ID=com.ohmydsp.OhMyDSP.IPC4Session
UUID=2C2DF9E1-4F38-32B3-B31B-54D151936299
LOGROOT=/Library/Logs/OhMyDSP/startup

fail() { echo "FAIL: $*" >&2; exit 1; }
secure_path() {
    entry=$1
    while [ "$entry" != / ]; do
        [ -e "$entry" ] && [ ! -L "$entry" ] || fail "missing or symlinked path: $entry"
        owner=$(stat -f %u "$entry"); mode=$(stat -f %Lp "$entry")
        [ "$owner" = 0 ] && [ $((0$mode & 022)) -eq 0 ] || fail "path must be root-owned and not group/world writable: $entry"
        entry=$(dirname "$entry")
    done
}
verify() {
    secure_path "$BASE/load-session.sh"
    secure_path "$BASE/pins.sha256"
    while IFS= read -r line; do
        secure_path "${line#*  }"
    done < "$BASE/pins.sha256"
    shasum -a 256 -c "$BASE/pins.sha256" || fail 'installed artifacts differ from qualified baseline'
    codesign --verify --deep --strict "$KEXT" || fail 'kext signature verification failed'
}
[ "$(id -u)" = 0 ] || fail 'run with sudo'
case "${1:-run}" in run|check) ;; *) fail 'usage: load-session.sh [check]';; esac
if [ "${1:-run}" = check ]; then
    verify
    echo 'Pinned startup artifacts verified'
    exit 0
fi

# Only root can create or replace entries here; existing symlinks are refused.
mkdir -p "$LOGROOT"
secure_path "$LOGROOT"
BOOT=$(sysctl -n kern.bootsessionuuid)
case "$BOOT" in ''|*[!A-Fa-f0-9-]*) fail 'invalid boot UUID';; esac
OUT=$(mktemp -d "$LOGROOT/$BOOT.XXXXXX")
chmod 755 "$OUT"
exec > "$OUT/loader.log" 2>&1
trap 'result=$?; echo "exit_status=$result"; printf "%s\n" "$result" > "$OUT/exit-status"' EXIT
echo "boot_uuid=$BOOT"
date -u '+started_utc=%Y-%m-%dT%H:%M:%SZ'
echo "loader_log=$OUT"
# Record verification failures in the receipt for this attempt.
verify
ARGS=$(sysctl -n kern.bootargs)
case " $ARGS " in *' -ohmydspoff '*) fail 'OhMyDSP disabled by boot argument';; esac
LOADED=$(kmutil showloaded)
OTHERS=$(printf '%s\n' "$LOADED" | grep 'com\.ohmydsp\.OhMyDSP' | grep -v "$ID (" || true)
[ -z "$OTHERS" ] || fail 'another OhMyDSP experiment is loaded'
if printf '%s\n' "$LOADED" | grep -F "$ID (" >/dev/null; then
    printf '%s\n' "$LOADED" | grep -F "$ID (" | grep -F "$UUID" >/dev/null || fail 'loaded session UUID differs'
    action=already_loaded
    echo 'expected session already loaded; no reload'
else
    action=loaded
    echo 'loading qualified session kext'
    # Keep a wedged kmutil invocation from leaving this job running indefinitely.
    kmutil load -p "$KEXT" & load_pid=$!
    count=0
    while kill -0 "$load_pid" 2>/dev/null; do
        if [ "$count" -ge 60 ]; then
            kill -TERM "$load_pid" 2>/dev/null || true
            sleep 2
            kill -KILL "$load_pid" 2>/dev/null || true
            wait "$load_pid" 2>/dev/null || true
            fail 'kmutil exceeded 120 seconds; no retry'
        fi
        sleep 2; count=$((count+1))
    done
    wait "$load_pid" || fail 'kmutil load failed; inspect approval/error output above'
fi
kmutil showloaded --bundle-identifier "$ID" > "$OUT/loaded.txt" 2>&1
grep -F "$ID (" "$OUT/loaded.txt" | grep -F "$UUID" >/dev/null || fail 'expected session is not loaded'
# Preserve the kext's existing delayed preparation; never issue START here.
count=0
while [ "$count" -lt 18 ]; do
    registry=$(ioreg -rc OhMyDSP -l -w0)
    printf '%s\n' "$registry" | awk 'index($0, "\"ipc4-pcm-capture\"") == 0' > "$OUT/ohmydsp-ioreg"
    if grep -q '"ipc4-session-ready" = Yes' "$OUT/ohmydsp-ioreg"; then
        echo "load_action=$action"
        echo 'PASS: session ready; recording remains client-controlled'
        exit 0
    fi
    grep -q '"ipc4-session-ready" = No' "$OUT/ohmydsp-ioreg" && fail 'session initialization failed'
    sleep 5; count=$((count+1))
done
fail 'session did not become ready within 90 seconds; no retry'
