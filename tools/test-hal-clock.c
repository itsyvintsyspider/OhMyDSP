#include "../ohmydsp/userspace/clock.h"
#include <stdio.h>
#include <assert.h>

static void run(double ppm,uint32_t ring,double nsPerTick)
{
    struct OMDClock c={0};
    const double base=2000000000000.0/nsPerTick,nominal=1e9/(48000*nsPerTick),trueStep=nominal*(1+ppm/1e6);
    uint64_t previousSample=0,previousHost=0;
    double maxError=0;
    // 30 minutes at 500 pump observations/s, including repeated anchors and hundreds of wraps.
    for(unsigned i=0;i<=900000;i++) {
        double now=base+i*2e6/nsPerTick;
        double hardware=(now-base)/trueStep;
        uint64_t lap=(uint64_t)fmax(0,hardware-96)/ring;
        double jitter=fmin(600000.0,ring/48000.0*1e9*.0005)*sin(lap*1.7);
        uint64_t host=(uint64_t)llround(base+lap*ring*trueStep+jitter/nsPerTick);
        assert(omdClockUpdate(&c,1,lap*ring,host,(uint64_t)now,nsPerTick));
        assert(c.sample%512==0 && c.sample>=previousSample && (uint64_t)c.host>=previousHost);
        assert(c.host<=now);
        if(c.sample==previousSample && previousHost)assert((uint64_t)c.host==previousHost);
        double trueFrame=(c.host-base)/trueStep,error=fabs(trueFrame-c.sample);
        if(error>maxError)maxError=error;
        assert(error<240); // <5 ms, comfortably within the unchanged 20 ms input safety offset.
        previousSample=c.sample;previousHost=(uint64_t)c.host;
    }
    printf("30min ring=%u drift=%+.0fppm ns/tick=%.3f: max mapping error %.3f frames, final rate %+.3fppm\n",ring,ppm,nsPerTick,maxError,(c.step/c.nominal-1)*1e6);
}
int main(void)
{
    run(0,131072,1);run(500,131072,1);run(-500,131072,1);run(200,16384,1);run(-200,16384,125.0/3);
    struct OMDClock c={0};uint64_t base=1000000000;
    assert(omdClockUpdate(&c,1,0,base,base,1));
    // Replay measured lap anchors from the failing machine; published pairs never jump at a wrap.
    uint64_t raw[4]={0,2730724479ULL,5460910519ULL,8192145235ULL};
    unsigned lap=0;uint64_t prevS=0,prevH=base;
    for(uint64_t t=0;t<10000000000ULL;t+=2000000) {
        if(lap<3&&t>raw[lap+1]+2000000)lap++;
        assert(omdClockUpdate(&c,1,lap*131072,base+raw[lap],base+t,1));
        assert(c.sample>=prevS && c.host>=prevH);
        if(c.sample!=prevS) {assert(c.sample-prevS==512);assert(fabs((c.host-prevH)/(512*(1e9/48000))-1)<.0011);}
        else assert((uint64_t)c.host==prevH);
        prevS=c.sample;prevH=(uint64_t)c.host;
    }
    assert(omdClockUpdate(&c,2,0,base,base,1)&&c.sample==0&&c.epoch==2);
    assert(!omdClockUpdate(&c,2,0,base+1,base+100,1)); // changing an existing anchor is invalid
    assert(omdClockUpdate(&c,3,0,base,base,1));
    assert(!omdClockUpdate(&c,3,131072,base+100,base+200,1)&&c.rejected==1);
    assert(omdClockUpdate(&c,4,0,base,base,1));
    assert(!omdClockUpdate(&c,4,0,base,base+20000000000ULL,1)); // bounded catch-up, not silent fabricated progress
    assert(omdClockUpdate(&c,5,0,base,base,1));
    assert(omdClockUpdate(&c,5,0,base,base+100000000,1)); // ordinary 100ms scheduling delay
    assert(omdClockUpdate(&c,6,0,base,base,1));
    assert(!omdClockUpdate(&c,6,0,base-1,base+1,1));
    puts("Clock replay, monotonicity, epoch reset, corrupt-anchor rejection and bounded catch-up PASS");
}
