// Boot-arg configuration of OhMyDSP, parsed once (ohmydspLoadConfig is idempotent). Documented in docs/boot-args.md.
//   -ohmydspoff       do not start at all (also honoured through Lilu's shouldLoad)     -ohmydspdbg   verbose runtime logging
//   ohmydsp_audio=N   0 no audio device, 1 synthetic tone, 2 live microphones (default 2; default 0 when ohmydsp_p2 is set)
//   ohmydsp_delay=N   seconds between start and the first register read (default 30)
//   ohmydsp_ring=N    DMA ring in KiB, multiple of 8 (default 128)       ohmydsp_sd=N     capture stream descriptor 0..6 (default 5)
//   ohmydsp_tick=N    position poll interval in ms (default 5)           ohmydsp_ts=N     0 timestamp at the exact wrap, 1 at "now"
//   ohmydsp_raw=1     no mic join: left = mic A, right = mic B           ohmydsp_gain=N   initial input gain in dB (default 9)
//   Phase 2-4 harness (experimental): ohmydsp_p2, ohmydsp_p2_keep, ohmydsp_p3_n, ohmydsp_p3_dump, ohmydsp_p4, ohmydsp_p4_sd (see DSPBoot.cpp)
#pragma once
#include <sys/types.h>

struct OhMyDSPConfig {
    bool off = false, debug = false, raw = false;
    int audio = 2, delay = 30, ringKiB = 128, sd = 5, tickMs = 5, tsMode = 0, gainDb = 9;
};
enum OhMyDSPLiluState { LiluStandalone = 0, LiluApproved = 1, LiluRefused = 2 };   // standalone: loaded by hand after boot, Lilu's API is closed

extern OhMyDSPConfig gCfg;
extern int gLiluState;
void ohmydspLoadConfig();
