// Single-writer device-clock estimator. Fixed publication period, hardware-locked rate,
// no revision of previously published sample/host pairs. Host units are mach ticks.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
// Default hardware experiment isolates the advertised period fix. The smoothed
// 512-frame path is retained only for offline comparison; live testing regressed.
#ifndef OHMYDSP_SMOOTH_CLOCK_EXPERIMENT
#define OMD_HAL_CLOCK_PERIOD 131072u
#define OMD_HAL_CLOCK_CONTRACT 3u
#else
#define OMD_HAL_CLOCK_PERIOD 512u
#define OMD_HAL_CLOCK_CONTRACT 2u
#endif
#define OMD_CLOCK_PERIOD 512u
#define OMD_CLOCK_CONTRACT 2u
struct OMDClock {
    bool initialized, failed;
    uint32_t epoch;
    uint64_t sample, lastRawSample, lastRawHost, advances, rejected;
    double host, nominal, step, filtered, target, maxPhaseNs;
};
static inline double omdClamp(double x,double lo,double hi){return x<lo?lo:x>hi?hi:x;}
static inline bool omdClockUpdate(struct OMDClock *c,uint32_t epoch,uint64_t rawSample,uint64_t rawHost,uint64_t now,double nsPerTick)
{
    if(!rawHost || !isfinite(nsPerTick) || nsPerTick<=0 || rawSample>(1ULL<<52))return false;
    if(!c->initialized || epoch!=c->epoch) {
        memset(c,0,sizeof(*c));c->initialized=true;c->epoch=epoch;
        c->nominal=1e9/(48000.0*nsPerTick);c->step=c->filtered=c->target=c->nominal;
        c->sample=rawSample-rawSample%OMD_CLOCK_PERIOD;
        c->host=(double)rawHost-(rawSample-c->sample)*c->nominal;
        c->lastRawSample=rawSample;c->lastRawHost=rawHost;
    }
    if(c->failed)return false;
    if((double)now<c->host || rawSample<c->lastRawSample || rawHost<c->lastRawHost) {c->failed=true;return false;}
    // Catch up using only previously selected rates. At most 10.9 seconds per call.
    unsigned count=0;
    while(count<1024) {
        double nextStep=omdClamp(c->target,c->step-c->nominal*1e-6,c->step+c->nominal*1e-6);
        double nextHost=c->host+OMD_CLOCK_PERIOD*nextStep;
        if(nextHost>(double)now)break;
        c->host=nextHost;c->sample+=OMD_CLOCK_PERIOD;c->step=nextStep;c->advances++;count++;
    }
    if(count==1024){c->failed=true;return false;}
    if(rawSample!=c->lastRawSample) {
        double observed=(double)(rawHost-c->lastRawHost)/(rawSample-c->lastRawSample);
        // Reject corrupt measurements instead of driving the output rate arbitrarily.
        if(observed<c->nominal*.998 || observed>c->nominal*1.002){c->rejected++;c->failed=true;return false;}
        c->filtered+=(observed-c->filtered)*.125;
        double predicted=c->host+((double)rawSample-(double)c->sample)*c->step;
        double phase=(double)rawHost-predicted;
        if(fabs(phase*nsPerTick)>c->maxPhaseNs)c->maxPhaseNs=fabs(phase*nsPerTick);
        // Correct phase over 20 seconds; frequency correction is limited to +/-1000 ppm.
        c->target=omdClamp(c->filtered+phase/(48000.0*20),c->nominal*.999,c->nominal*1.001);
        c->lastRawSample=rawSample;c->lastRawHost=rawHost;
    } else if(rawHost!=c->lastRawHost) {c->failed=true;return false;}
    return true;
}
