# Phase 6.1c userspace: build/ohmydspd (root daemon) and build/OhMyDSP.driver (AudioServerPlugIn). Installed by tools/install-userspace.sh.
CC := cc
CFLAGS := -O2 -Wall -Wextra -arch x86_64 -mmacosx-version-min=11.0 -fblocks -std=gnu11
all: build/ohmydspd build/OhMyDSP.driver
build/ohmydspd: daemon/ohmydspd.c protocol.h ../include/OhMyDSPShared.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ daemon/ohmydspd.c -framework IOKit -framework CoreFoundation
	codesign -f -s - $@
build/OhMyDSP.driver: driver/OhMyDSPDriver.c driver/Info.plist protocol.h
	rm -rf $@; mkdir -p $@/Contents/MacOS
	cp driver/Info.plist $@/Contents/Info.plist
	$(CC) $(CFLAGS) -bundle -o $@/Contents/MacOS/OhMyDSPDriver driver/OhMyDSPDriver.c -framework CoreAudio -framework CoreFoundation
	codesign -f -s - $@
clean:
	rm -rf build
.PHONY: all clean
