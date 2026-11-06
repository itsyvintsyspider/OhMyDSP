#!/bin/sh
# One mic-A experiment cycle (run with sudo, from the repo root).  usage: sudo sh tools/mica-run.sh [reset | field=value ...]
# Needs the kext build that has the "raw" tunable and the "chan:" log line, local/engine-tune and local/recordtest.
set -e
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
U="${SUDO_USER:-root}"
sh tools/load-kext.sh unload || true
if [ -n "$1" ]; then python3 -I tools/mica-exp.py "$@"; else python3 -I tools/mica-exp.py show; fi
sh tools/load-kext.sh
echo "waiting 45 s for the delayed probe..."; sleep 45
local/engine-tune raw=1 gain_db=7
echo ">>> MAKE NOISE NOW (talk, clap, tap near the top bezel) for 6 s <<<"; printf '\a'
sudo -u "$U" local/recordtest 6 OhMyDSP local/mica-raw.bin | grep -E "IOProc|ch0|ch1"
sleep 3
echo "--- per-channel log (A = left, B = right, raw S32 before gain):"
/usr/bin/log show --last 40s --info --debug --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS "chan:"' 2>/dev/null | grep "chan:" | sed 's/.*chan: //' | tail -4
