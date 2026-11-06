# OhMyDSP kext build: IOKit + IOAudioFamily driver.
#   make            LILU=1 (default): Lilu plugin, depends on as.vit9696.Lilu, built into build/lilu/  -> inject with OpenCore (Kernel > Add, after Lilu)
#   make CORE=1     (combine with LILU=0/1) Phase 6 core kext without IOAudioFamily -> build/core-lilu, build/core-standalone
#   make LILU=0     standalone, no Lilu dependency,             built into build/standalone/ -> kmutil load / /Library/Extensions (tools/load-kext.sh)
# Both share all sources; LILU=0 only skips src/kern_start.cpp's Lilu policy (-DOHMYDSP_NO_LILU). Lilu SDK headers: third_party/Lilu.
LILU    ?= 1
VERSION := 0.3.0
SDK     := $(shell xcrun --sdk macosx --show-sdk-path)
KFW     := $(SDK)/System/Library/Frameworks/Kernel.framework
CXX     := $(shell xcrun --find clang++)
RES     := $(shell $(CXX) -print-resource-dir)
ifeq ($(LILU),1)
VARIANT := lilu
LILUFLAGS := -I third_party/Lilu -D__ACIDANTHERA_MAC_SDK -DPRODUCT_NAME=OhMyDSP -DMODULE_VERSION=$(VERSION)
else
VARIANT := standalone
LILUFLAGS := -DOHMYDSP_NO_LILU
endif
# CORE=1: no IOAudioFamily (Phase 6): capture session + root-only user client, injectable into the Boot KC. Audio lives in userspace.
CORE    ?= 0
DROP    := $(if $(filter 0,$(LILU)),as.vit9696.Lilu|)$(if $(filter 1,$(CORE)),IOAudioFamily,IOUserClientClass)
ifeq ($(CORE),1)
VARIANT := core-$(VARIANT)
COREFLAGS := -DOHMYDSP_CORE
CSRCS := src/Capture.cpp src/UserClient.cpp
CHDRS := src/Capture.hpp src/UserClient.hpp include/OhMyDSPShared.h
else
CSRCS := src/MicAudio.cpp
CHDRS := src/MicAudio.hpp
endif
OUT     := build/$(VARIANT)/OhMyDSP.kext
CXXFLAGS := -arch x86_64 -mmacosx-version-min=11.0 -std=gnu++17 -fno-rtti -fno-exceptions -fno-builtin \
            -fapple-kext -mkernel -nostdinc -nostdinc++ -O2 -Wall -Wno-deprecated-declarations \
            -isystem $(RES)/include -I$(KFW)/Headers -I$(KFW)/PrivateHeaders $(LILUFLAGS) $(COREFLAGS) -DOHMYDSP_VERSION=$(VERSION) \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
LDFLAGS  := -arch x86_64 -mmacosx-version-min=11.0 -Xlinker -kext -nostdlib -L$(SDK)/usr/lib -lkmodc++ -lkmod /Library/Developer/CommandLineTools/usr/lib/clang/16.0.0/lib/darwin/libclang_rt.cc_kext.a

all: $(OUT)
SRCS := src/OhMyDSP.cpp src/DSPBoot.cpp src/DSPDevice.cpp $(CSRCS) src/Config.cpp src/kern_start.cpp
HDRS := src/OhMyDSP.hpp $(CHDRS) src/DSPDevice.hpp src/Config.hpp
$(OUT): $(SRCS) $(HDRS) Info.plist Makefile
	rm -rf $(OUT)
	mkdir -p $(OUT)/Contents/MacOS
	grep -Ev '$(DROP)' Info.plist > $(OUT)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(OUT)/Contents/MacOS/OhMyDSP $(LDFLAGS)
	codesign -f -s - $(OUT)

clean:
	rm -rf build
.PHONY: all clean
