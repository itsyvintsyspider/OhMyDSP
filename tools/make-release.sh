#!/bin/sh
# Build and package a release: dist/OhMyDSP-<version>.zip (kext + preference pane + licence) and dist/SHA256SUMS.
# Usage: sh tools/make-release.sh        (run from the repo root; needs the Xcode Command Line Tools)
set -e
cd "$(dirname "$0")/.."
V=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" ohmydsp/Info.plist)
[ "$V" = "$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" ohmydsp/prefpane/Info.plist)" ] || { echo "kext and pane versions differ"; exit 1; }
make -C ohmydsp CORE=1 && make -C ohmydsp/userspace && make -C ohmydsp/prefpane
S="dist/OhMyDSP-$V"; rm -rf dist; mkdir -p "$S"
cp -R ohmydsp/build/core-lilu/OhMyDSP.kext ohmydsp/prefpane/build/OhMyDSP.prefPane "$S/"
cp LICENSE "$S/"
cat > "$S/INSTALL.txt" <<T
OhMyDSP $V - internal digital microphones for Comet Lake-H hackintoshes (Intel DSP / SOF).

1. Copy OhMyDSP.kext to EFI/OC/Kexts and add it to config.plist > Kernel > Add, AFTER Lilu. Restart.
2. Double-click OhMyDSP.prefPane, open OhMyDSP in System Settings and press Install...
   (if macOS blocks it: right-click > Open, or run: xattr -dr com.apple.quarantine OhMyDSP.prefPane)
3. Restart once, then pick "Internal Microphone (DMIC)" in System Settings > Sound > Input.

Full guide, boot-args and safety notes: https://github.com/itsyvintsyspider/OhMyDSP
T
(cd dist && ditto -c -k --keepParent "OhMyDSP-$V" "OhMyDSP-$V.zip" && shasum -a 256 "OhMyDSP-$V.zip" > SHA256SUMS && cat SHA256SUMS)
