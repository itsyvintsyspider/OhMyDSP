// Module entry of OhMyDSP and its Lilu registration. OhMyDSP is an ordinary IOKit driver (PCI match, own audio device); Lilu supplies the boot
// policy every Lilu plugin shares: -liluoff / safe mode / installer and recovery are refused, the kernel range is enforced, -ohmydspoff disables,
// -ohmydspdbg turns debugging on. No Lilu patches or callbacks are registered, so (unlike patching plugins) this kext stays unloadable.
// Two builds from the same sources (see Makefile): LILU=1 depends on Lilu and is injected by OpenCore; LILU=0 (OHMYDSP_NO_LILU) has no Lilu
// dependency, runs "standalone" (boot-args applied by Config.cpp only) and can be loaded with kmutil or installed in /Library/Extensions.
#include <mach/kmod.h>
#include "Config.hpp"
#include <IOKit/IOLib.h>
#define OHMYDSP_STR2(x) #x
#define OHMYDSP_STR(x) OHMYDSP_STR2(x)

#ifndef OHMYDSP_NO_LILU
#define LILU_CUSTOM_KMOD_INIT
#define LILU_CUSTOM_IOKIT_INIT
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <mach/kmod.h>
#include "Config.hpp"

bool ADDPR(debugEnabled) = false;
uint32_t ADDPR(debugPrintDelay) = 0;

static const char *kOffArgs[] = { "-ohmydspoff" };
static const char *kDbgArgs[] = { "-ohmydspdbg" };
static const char *kBetaArgs[] = { "-ohmydspbeta" };

static void liluPolicy()
{
    LiluAPI::Error err = lilu.requestAccess();
    if (err == LiluAPI::Error::NoError) {
        err = lilu.shouldLoad(xStringify(PRODUCT_NAME), parseModuleVersion(xStringify(MODULE_VERSION)), LiluAPI::AllowNormal,
                              kOffArgs, 1, kDbgArgs, 1, kBetaArgs, 1, KernelVersion::BigSur, KernelVersion::Tahoe, ADDPR(debugEnabled));
        lilu.releaseAccess();
        gLiluState = (err == LiluAPI::Error::NoError) ? LiluApproved : LiluRefused;
        IOLog("OhMyDSP: Lilu %s (code %d)\n", gLiluState == LiluApproved ? "approved the load" : "refused the load", (int)err);
    } else {
        gLiluState = LiluStandalone;
        IOLog("OhMyDSP: Lilu access unavailable (code %d), running standalone\n", (int)err);
    }
    if (gCfg.debug) ADDPR(debugEnabled) = true;
}

#else
static void liluPolicy() { gLiluState = LiluStandalone; }
#endif

extern "C" kern_return_t ohmydsp_kern_start(kmod_info_t *, void *)
{
    ohmydspLoadConfig();
    liluPolicy();
    return KERN_SUCCESS;     // nothing to keep alive: IOKit matching does the rest
}
extern "C" kern_return_t ohmydsp_kern_stop(kmod_info_t *, void *) { return KERN_SUCCESS; }

// kmod info (what Xcode generates as <name>_info.c). libkmodc++'s _start/_stop run the static constructors and call _realmain/_antimain.
extern "C" {
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.ohmydsp.OhMyDSP, OHMYDSP_STR(OHMYDSP_VERSION), _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = ohmydsp_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = ohmydsp_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
