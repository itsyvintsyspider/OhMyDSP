#!/bin/sh
# Phase 6.1 verification: recording through daemon + driver survives a daemon kill, a kext reload and an idle system sleep.
# Run from the repo root as:   sudo sh tools/verify-6.1.sh        (needs local/recordtest: cc -O2 -o local/recordtest tools/recordtest.c -framework CoreAudio -framework CoreFoundation)
# Total time about 2 minutes. Results go to local/verify-6.1.txt.
# The sleep / wake step is OFF by default (a failed wake needs a hard power cycle): run `sudo SLEEP=1 sh tools/verify-6.1.sh` to include it,
# and only when you can reach the power button and nothing unsaved is open.
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
USR="${SUDO_USER:-root}"; OUT=local/verify-6.1.txt; : > "$OUT"
PASS=0; FAIL=0
say() { echo "$*" | tee -a "$OUT"; }
rec() { sudo -u "$USR" local/recordtest "$1" OhMyDSP 2>&1 | grep -E "IOProc calls|start failed|no input" | tee -a "$OUT"; }
nz() { echo "$1" | sed -n 's/.*non-zero samples \([0-9]*\).*/\1/p'; }
frames() { echo "$1" | sed -n 's/.*frames \([0-9]*\) (.*/\1/p'; }
check() { if [ "$2" = ok ]; then say "  PASS: $1"; PASS=$((PASS+1)); else say "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
since() { /usr/bin/log show --start "$1" --process kernel --predicate 'eventMessage CONTAINS "OhMyDSP"' 2>/dev/null | grep -E "$2" | sed -E 's/^([0-9-]+ [0-9:.]+)[^(]*\(OhMyDSP\) /\1 /' | cut -c1-150; }
now() { date '+%Y-%m-%d %H:%M:%S'; }

say "== 0. baseline (5 s recording) =="
R=$(rec 5); [ "$(nz "$R")" -gt 100000 ] 2>/dev/null && check "baseline has audio" ok || check "baseline has audio" no

say "== 1. daemon killed during a 12 s recording =="
T=$(now); sudo -u "$USR" local/recordtest 12 OhMyDSP > /tmp/v61-kill.txt 2>&1 &
RP=$!; sleep 5; launchctl kill SIGKILL system/com.ohmydsp.daemon; say "  daemon killed at +5 s"; wait $RP
R=$(grep -E "IOProc calls|start failed" /tmp/v61-kill.txt); say "  $R"
[ -n "$R" ] && check "recording ended on its own (no hang)" ok || check "recording ended on its own (no hang)" no
sleep 2; since "$T" "engine stop" | tail -2 | tee -a "$OUT"
since "$T" "engine stop: DMA stop OK" | grep -q . && check "DSP stopped after the daemon died" ok || check "DSP stopped after the daemon died" no
pgrep -x ohmydspd > /dev/null && check "launchd restarted the daemon" ok || check "launchd restarted the daemon" no
R=$(rec 5); [ "$(nz "$R")" -gt 100000 ] 2>/dev/null && check "recording works again afterwards" ok || check "recording works again afterwards" no

say "== 2. kext reload =="
sh tools/load-kext.sh unload > /dev/null 2>&1; sh tools/load-kext.sh core 2>&1 | grep -E "ohmydsp|NOT LOADED" | tee -a "$OUT"
sh tools/install-userspace.sh > /dev/null 2>&1; say "  waiting 40 s for the kext to prepare the DSP (ohmydsp_delay)"; sleep 40
R=$(rec 5); [ "$(nz "$R")" -gt 100000 ] 2>/dev/null && check "recording works after the reload" ok || check "recording works after the reload" no

if [ "${SLEEP:-0}" = 1 ]; then
say "== 3. idle sleep / wake (wake the machine with a key press) =="
T=$(now); pmset sleepnow; sleep 20
R=$(rec 5); [ "$(nz "$R")" -gt 100000 ] 2>/dev/null && check "recording works after wake" ok || check "recording works after wake" no
since "$T" "FAULT|ROMDIAG|TIMEOUT|FAILED" | tee -a "$OUT"
since "$T" "FAULT|ROMDIAG|TIMEOUT|FAILED" | grep -q . && check "no kernel-side faults around the sleep" no || check "no kernel-side faults around the sleep" ok

else
  say "== 3. idle sleep / wake: skipped (run with SLEEP=1 to include) =="
fi

say "== result: $PASS passed, $FAIL failed (details in $OUT) =="
