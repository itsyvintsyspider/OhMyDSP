#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$ROOT/local"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$ROOT/local/coreaudio-stability" \
    "$ROOT/tools/coreaudio-stability.c" -Wl,-sectcreate,__TEXT,__info_plist,"$ROOT/tools/coreaudio-stability-info.plist" -framework CoreAudio -framework CoreFoundation
codesign -f -s - "$ROOT/local/coreaudio-stability"
"$ROOT/local/coreaudio-stability" --self-test
