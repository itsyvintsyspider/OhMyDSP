// Exercise actual worker lifetime with in-memory rings, without realtime privileges or a kext.
#include <mach/mach_time.h>
static kern_return_t testWaitUntil(uint64_t deadline);
#define main daemon_main_unused
#define thread_policy_set testThreadPolicy
#define mach_wait_until testWaitUntil
#include "../ohmydsp/userspace/daemon/ohmydspd.c"
#undef main
#undef thread_policy_set
#undef mach_wait_until
#include <assert.h>
static dispatch_semaphore_t firstPoll;
static unsigned waitCalls;
static kern_return_t testWaitUntil(uint64_t deadline)
{
    // The second wait is reached only after the worker completes its first pump.
    if (++waitCalls == 2) dispatch_semaphore_signal(firstPoll);
    return mach_wait_until(deadline);
}
static kern_return_t policyResult;
kern_return_t testThreadPolicy(thread_act_t t, thread_policy_flavor_t flavor,
                              thread_policy_t info, mach_msg_type_number_t count)
{
    (void)t;
    assert(flavor==THREAD_TIME_CONSTRAINT_POLICY && count==THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    thread_time_constraint_policy_t p=(thread_time_constraint_policy_t)info;
    assert(p->computation>0 && p->computation<p->constraint && p->constraint<p->period && p->preemptible);
    return policyResult;
}
int main(void)
{
    struct OhMyDSPStatus status={.state=OHMYDSP_STATE_RUNNING,.epoch=1};
    struct OhMyDSPShm shared={0};
    mach_timebase_info_data_t tb;mach_timebase_info(&tb);diagNsPerTick=(double)tb.numer/tb.denom;
    ringBytes=1048576;frameBytes=8;kst=&status;shm=&shared;
    kring=calloc(1,ringBytes);sring=calloc(1,ringBytes);assert(kring&&sring);
    status.lapHostTime=mach_absolute_time();
    for(unsigned i=0;i<2;i++) {
        policyResult=i?KERN_FAILURE:KERN_SUCCESS;
        diagPolls=0;lastEpoch=(uint32_t)-1;written=0;pos=0;
        atomic_store(&pumpQuit,false);
        waitCalls=0;
        firstPoll=dispatch_semaphore_create(0);
        assert(pthread_create(&pumpThread,NULL,pumpThreadMain,NULL)==0);pumpJoinable=true;
        assert(dispatch_semaphore_wait(firstPoll,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))==0);
        stopPump();
        dispatch_release(firstPoll);
        assert(!pumpJoinable && atomic_load(&pumpQuit) && diagPolls>0 && pumpPolicyStatus==policyResult);
        uint64_t polls=diagPolls;stopPump();assert(diagPolls==polls);
    }
    free((void *)kring);free(sring);
    puts("Dedicated pump: start, join, idempotent stop, restart and policy failure reporting PASS");
}
