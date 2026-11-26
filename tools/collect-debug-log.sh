#!/bin/sh
# Collect diagnostics for an OhMyDSP experiment. Read-only with respect to
# hardware. It does not load, unload, install, or stop any driver or daemon.
set -u

umask 077
ROOT="${1:-$(mktemp -d /private/tmp/ohmydsp-debug.XXXXXX)}"
mkdir -p "$ROOT"

run() {
    name="$1"
    shift
    "$@" >"$ROOT/$name" 2>&1 || true
}

# Older probe builds retained PCM as a hex property. Exclude it before writing any dump.
registry() {
    ioreg "$@" | awk 'index($0, "\"ipc4-pcm-capture\"") == 0'
}

run timestamp date -u
run uname uname -a
run boot-args nvram boot-args
run audio system_profiler SPAudioDataType
run pci registry -p IOService -n AppleHDAController -l -w0
run io-device-tree registry -p IODeviceTree -l -w0
run ohmydsp-ioreg registry -rc OhMyDSP -l -w0
run kexts kmutil showloaded
run kernel-log log show --last 15m --style syslog --predicate 'eventMessage CONTAINS[c] "OhMyDSP"'

printf '%s\n' "saved diagnostics to $ROOT"
