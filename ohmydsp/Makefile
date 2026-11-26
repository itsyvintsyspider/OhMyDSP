# OhMyDSP kext build: IOKit + IOAudioFamily driver.
#   make            LILU=1 (default): Lilu plugin, depends on as.vit9696.Lilu, built into build/lilu/  -> inject with OpenCore (Kernel > Add, after Lilu)
#   make CORE=1     (combine with LILU=0/1) Phase 6 core kext without IOAudioFamily -> build/core-lilu, build/core-standalone
#   make LILU=0     standalone, no Lilu dependency,             built into build/standalone/ -> kmutil load / /Library/Extensions (tools/load-kext.sh)
# Both share all sources; LILU=0 only skips src/kern_start.cpp's Lilu policy (-DOHMYDSP_NO_LILU). Lilu SDK headers: third_party/Lilu.
LILU    ?= 1
RPL     ?= 0
LIVE    ?= 0
MANUAL  ?= 0
DISCOVER ?= 0
PROBE   ?= 0
MODULE  ?= 0
BIND    ?= 0
CAPTURE ?= 0
CAPTURE_DMA ?= 0
SESSION ?= 0
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
ifeq ($(RPL),1)
RPLFLAGS := -DOHMYDSP_RPL_EXPERIMENTAL
RPLMATCH := 0x51ca8086
else
RPLFLAGS :=
RPLMATCH := 0x06c88086
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE))),1)
RPLFLAGS += -DOHMYDSP_RPL_LIVE
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(MANUAL))),1)
RPLFLAGS += -DOHMYDSP_RPL_MANUAL
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(DISCOVER))),1)
RPLFLAGS += -DOHMYDSP_RPL_DISCOVER
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(PROBE))),1)
RPLFLAGS += -DOHMYDSP_RPL_IPC4_PROBE
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(MODULE))),1)
RPLFLAGS += -DOHMYDSP_RPL_IPC4_MODULE_PROBE
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(BIND))),1)
RPLFLAGS += -DOHMYDSP_RPL_IPC4_BIND_PROBE
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE))),1)
RPLFLAGS += -DOHMYDSP_RPL_IPC4_CAPTURE_PROBE
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE_DMA))),1)
RPLFLAGS += -DOHMYDSP_RPL_IPC4_CAPTURE_DMA_PROBE
endif
ifeq ($(SESSION),1)
ifneq ($(RPL)$(LIVE)$(MANUAL)$(CORE),1111)
$(error SESSION=1 requires RPL=1 LIVE=1 MANUAL=1 CORE=1)
endif
ifneq ($(DISCOVER)$(PROBE)$(MODULE)$(BIND)$(CAPTURE)$(CAPTURE_DMA),000000)
$(error SESSION cannot be combined with probe variants)
endif
RPLFLAGS += -DOHMYDSP_RPL_IPC4_SESSION
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(PROBE))),1)
PLIST_ID_SED := -e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4Probe/g'
else ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(BIND))),1)
PLIST_ID_SED := -e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4BindProbe/g'
else ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(MODULE))),1)
PLIST_ID_SED := -e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4ModuleProbe/g'
else ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE))),1)
PLIST_ID_SED := -e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4CaptureProbe/g'
else ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE_DMA))),1)
PLIST_ID_SED := -e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4CaptureDMAProbe/g'
else
PLIST_ID_SED := $(if $(filter 1,$(SESSION)),-e 's/com.ohmydsp.OhMyDSP/com.ohmydsp.OhMyDSP.IPC4Session/g',)
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
ifeq ($(RPL),1)
VARIANT := rpl-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE))),1)
VARIANT := live-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(MANUAL))),1)
VARIANT := manual-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(DISCOVER))),1)
VARIANT := discover-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(PROBE))),1)
VARIANT := probe-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(MODULE))),1)
VARIANT := module-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(BIND))),1)
VARIANT := bind-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE))),1)
VARIANT := capture-$(VARIANT)
endif
ifeq ($(and $(filter 1,$(RPL)),$(filter 1,$(LIVE)),$(filter 1,$(CAPTURE_DMA))),1)
VARIANT := capture-dma-$(VARIANT)
endif
ifeq ($(SESSION),1)
VARIANT := session-$(VARIANT)
endif
OUT     := build/$(VARIANT)/OhMyDSP.kext
CXXFLAGS := -arch x86_64 -mmacosx-version-min=11.0 -std=gnu++17 -fno-rtti -fno-exceptions -fno-builtin \
            -fapple-kext -mkernel -nostdinc -nostdinc++ -O2 -Wall -Wno-deprecated-declarations \
            -isystem $(RES)/include -I$(KFW)/Headers -I$(KFW)/PrivateHeaders $(LILUFLAGS) $(COREFLAGS) $(RPLFLAGS) -DOHMYDSP_VERSION=$(VERSION) \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
LDFLAGS  := -arch x86_64 -mmacosx-version-min=11.0 -Xlinker -kext -nostdlib -L$(SDK)/usr/lib -lkmodc++ -lkmod /Library/Developer/CommandLineTools/usr/lib/clang/16.0.0/lib/darwin/libclang_rt.cc_kext.a

all: $(OUT)
SRCS := src/OhMyDSP.cpp src/DSPBoot.cpp src/DSPDevice.cpp $(CSRCS) src/Config.cpp src/kern_start.cpp
HDRS := src/OhMyDSP.hpp $(CHDRS) src/DSPDevice.hpp src/Config.hpp
$(OUT): $(SRCS) $(HDRS) Info.plist Makefile
	rm -rf $(OUT)
	mkdir -p $(OUT)/Contents/MacOS
	grep -Ev '$(DROP)' Info.plist | sed $(PLIST_ID_SED) -e 's/0x06c88086/$(RPLMATCH)/' > $(OUT)/Contents/Info.plist
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(OUT)/Contents/MacOS/OhMyDSP $(LDFLAGS)
	codesign -f -s - $(OUT)

clean:
	rm -rf build
.PHONY: all clean
