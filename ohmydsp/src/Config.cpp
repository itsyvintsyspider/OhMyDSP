#include "Config.hpp"
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

OhMyDSPConfig gCfg;
int gLiluState = LiluStandalone;
static bool gLoaded = false;

static bool has(const char *name) { int v[4]; return PE_parse_boot_argn(name, v, sizeof v); }

static int arg(const char *name, int def, int lo, int hi)
{
    int v = 0;
    if (!PE_parse_boot_argn(name, &v, sizeof v)) return def;
    if (v < lo || v > hi) { IOLog("OhMyDSP: boot-arg %s=%d is outside %d..%d, using %d\n", name, v, lo, hi, def); return def; }
    return v;
}

void ohmydspLoadConfig()
{
    if (gLoaded) return;
    gLoaded = true;
    OhMyDSPConfig &c = gCfg;
    c.off = has("-ohmydspoff");
    c.debug = has("-ohmydspdbg");
    c.audio = arg("ohmydsp_audio", has("ohmydsp_p2") ? 0 : 2, 0, 2);
    c.delay = arg("ohmydsp_delay", 30, 1, 600);
    c.ringKiB = arg("ohmydsp_ring", 128, 16, 1024) & ~7;
    c.sd = arg("ohmydsp_sd", 5, 0, 6);
    c.tickMs = arg("ohmydsp_tick", 5, 1, 20);
    c.tsMode = arg("ohmydsp_ts", 0, 0, 1);
    c.raw = arg("ohmydsp_raw", 0, 0, 1) != 0;
    c.gainDb = arg("ohmydsp_gain", 9, -28, 21);
    IOLog("OhMyDSP: config: audio=%d delay=%d s ring=%d KiB sd=%d tick=%d ms ts=%d raw=%d gain=%d dB debug=%d off=%d\n",
          c.audio, c.delay, c.ringKiB, c.sd, c.tickMs, c.tsMode, (int)c.raw, c.gainDb, (int)c.debug, (int)c.off);
}
