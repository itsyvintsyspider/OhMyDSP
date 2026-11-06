# OhMyDSP preference pane (Swift, swiftc only). Output: build/OhMyDSP.prefPane, self-contained: the userspace payload (daemon, driver) and the
# installer script live in its Resources, so the pane can install them with one admin prompt. Build ../userspace first: make -C ../userspace
OUT := build/OhMyDSP.prefPane
all: $(OUT)
build/icon.icns: makeicon.swift
	mkdir -p build/icon.iconset
	swift makeicon.swift build/icon1024.png
	for s in 16 32 64 128 256 512; do sips -z $$s $$s build/icon1024.png --out build/icon.iconset/icon_$${s}x$${s}.png >/dev/null; sips -z $$((s*2)) $$((s*2)) build/icon1024.png --out build/icon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; done
	iconutil -c icns build/icon.iconset -o $@
$(OUT): Sources/Pane.swift Sources/Audio.swift Sources/Report.swift Sources/Platform.swift Sources/Firmware.swift Info.plist build/icon.icns ../userspace/build/ohmydspd ../../tools/install-userspace.sh ../data/dmic-p10.ipc
	rm -rf $@; mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp Info.plist $@/Contents/Info.plist
	cp build/icon.icns $@/Contents/Resources/OhMyDSP.icns
	cp ../userspace/build/ohmydspd $@/Contents/Resources/; cp -R ../userspace/build/OhMyDSP.driver $@/Contents/Resources/; cp ../../tools/install-userspace.sh ../data/dmic-p10.ipc $@/Contents/Resources/
	swiftc -O -target x86_64-apple-macos13.0 -module-name OhMyDSPPane -emit-library -Xlinker -bundle -o $@/Contents/MacOS/OhMyDSPPane Sources/Pane.swift Sources/Audio.swift Sources/Report.swift Sources/Platform.swift Sources/Firmware.swift -framework PreferencePanes
	codesign -f -s - $@
clean:
	rm -rf build
.PHONY: all clean
