// Bounded, allocation-free observations shared by HAL and offline tests. No PCM is retained.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include "protocol.h"
#define OMD_DIAG_EVENTS 64
#define OMD_SNAPSHOT_TRIES 128
static inline bool omdSnapshot(const volatile struct OhMyDSPShm *h, struct OMDPosition *p)
{
    for (unsigned i=0;i<OMD_SNAPSHOT_TRIES;i++) {
        uint32_t seq=h->seq;
        __sync_synchronize();
        struct OMDPosition v;
        if (seq & 1) {
            if (h->nextSeq != seq) continue;
            v = h->nextPosition;
        } else {
            v = (struct OMDPosition){h->epoch,h->state,h->laps,h->lapHostTime,h->written,h->clockSample,h->clockHost,h->clockValid};
        }
        __sync_synchronize();
        if (seq==h->seq) { *p=v; return true; }
    }
    return false;
}
struct OMDMissing { uint64_t negative, future, stale, steady; };
static inline uint64_t omdIntersect(int64_t first, int64_t end, int64_t lo, int64_t hi)
{ int64_t a=first>lo?first:lo,b=end<hi?end:hi; return b>a?(uint64_t)(b-a):0; }
// Classifies exactly the same availability bounds used by DoIOOperation (480-frame overwrite guard).
static inline struct OMDMissing omdMissing(int64_t first, uint32_t frames, int64_t written, int64_t ring)
{
    int64_t end=first+frames, oldest=written-ring+480;
    if (oldest<0) oldest=0;
    struct OMDMissing m={0};
    m.negative=omdIntersect(first,end,INT64_MIN,0);
    m.future=omdIntersect(first,end,written,INT64_MAX);
    m.stale=omdIntersect(first,end,0,oldest);
    m.steady=omdIntersect(first,end,written>24000?written:24000,INT64_MAX)+omdIntersect(first,end,24000,oldest);
    return m;
}
struct OMDHalEvent {
    unsigned kind; // 1 = unavailable input, 2 = clock anchor; details counts or signed residual ns
    struct OMDPosition p; int64_t first; uint32_t frames;
    uint64_t negative, future, stale; double residualNs;
};
struct OMDHalDiag {
    uint64_t reads, requested, negative, future, stale, stateFrames, snapshotFrames, steadyMissing;
    uint64_t clockCalls, clockSnapshotFailures, anchors, clockRegressions;
    double maxAbsResidualNs;
    bool haveAnchor; struct OMDPosition previous;
    unsigned count, dropped; struct OMDHalEvent events[OMD_DIAG_EVENTS];
};
static inline void omdEvent(struct OMDHalDiag *d, struct OMDHalEvent e)
{ if(d->count<OMD_DIAG_EVENTS)d->events[d->count++]=e;else d->dropped++; }
static inline void omdRead(struct OMDHalDiag *d, const struct OMDPosition *p, int64_t first, uint32_t frames, int64_t ring, bool snapshot)
{
    d->reads++;d->requested+=frames;
    if(!snapshot) {d->snapshotFrames+=frames;return;}
    if(p->state!=1) {d->stateFrames+=frames;omdEvent(d,(struct OMDHalEvent){.kind=1,.p=*p,.first=first,.frames=frames});return;}
    struct OMDMissing m=omdMissing(first,frames,(int64_t)p->written,ring);
    d->negative+=m.negative;d->future+=m.future;d->stale+=m.stale;d->steadyMissing+=m.steady;
    if(m.negative||m.future||m.stale)omdEvent(d,(struct OMDHalEvent){.kind=1,.p=*p,.first=first,.frames=frames,.negative=m.negative,.future=m.future,.stale=m.stale});
}
static inline void omdClock(struct OMDHalDiag *d, const struct OMDPosition *p, uint32_t ring, double nsPerTick)
{
    d->clockCalls++;
    if(d->haveAnchor && p->epoch==d->previous.epoch && p->laps==d->previous.laps && p->host==d->previous.host)return;
    double residual=0;
    if(d->haveAnchor && p->epoch==d->previous.epoch) {
        double actual=p->host>=d->previous.host?(double)(p->host-d->previous.host):-(double)(d->previous.host-p->host);
        double laps=p->laps>=d->previous.laps?(double)(p->laps-d->previous.laps):-(double)(d->previous.laps-p->laps);
        residual=actual*nsPerTick-laps*ring*1e9/48000.0;
        if(p->laps<d->previous.laps||p->host<d->previous.host)d->clockRegressions++;
        if(fabs(residual)>d->maxAbsResidualNs)d->maxAbsResidualNs=fabs(residual);
    }
    d->anchors++;d->previous=*p;d->haveAnchor=true;
    omdEvent(d,(struct OMDHalEvent){.kind=2,.p=*p,.residualNs=residual});
}
