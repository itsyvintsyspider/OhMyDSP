// Explicit IPC4 user-client acceptance test. Does not install HAL or change defaults.
// cc -O2 -Wall -o local/ipc4-session-test tools/ipc4-session-test.c -framework IOKit -framework CoreFoundation
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../ohmydsp/include/OhMyDSPShared.h"

struct position { uint32_t epoch, state, lp; uint64_t frames, faults; };
static int snapshot(const volatile struct OhMyDSPStatus *s, struct position *p) {
    for (unsigned i=0;i<10000;i++) {
        uint32_t a=s->seq; __sync_synchronize();
        uint64_t laps=s->laps; p->lp=s->lpib; p->state=s->state; p->epoch=s->epoch; p->faults=s->faults;
        __sync_synchronize(); uint32_t b=s->seq;
        if (!(a&1) && a==b) {
            if (p->lp>=s->ringBytes) return 0;
            p->frames=laps*(s->ringBytes/8)+p->lp/8; return 1;
        }
    }
    return 0;
}
static double seconds(uint64_t t) { mach_timebase_info_data_t b; mach_timebase_info(&b); return (double)t*b.numer/b.denom/1e9; }
static int wav(const char *path, const void *data, uint32_t bytes) {
    FILE *f=fopen(path,"wx"); if(!f)return 0;
    uint32_t header[]={0x46464952,36+bytes,0x45564157,0x20746d66,16,0x00020001,48000,384000,0x00200008,0x61746164,bytes};
    int ok=fwrite(header,1,sizeof(header),f)==sizeof(header) && fwrite(data,1,bytes,f)==bytes;
    if(fclose(f))ok=0;return ok;
}
int main(int argc,char **argv) {
    if(argc!=4){fprintf(stderr,"usage: %s seconds cycles NEW_OUTPUT_DIRECTORY\n",argv[0]);return 2;}
    char *end; long duration=strtol(argv[1],&end,10); if(*end||duration<1||duration>60)return 2;
    long cycles=strtol(argv[2],&end,10); if(*end||cycles<1||cycles>10)return 2;
    if(mkdir(argv[3],0700)){perror("output directory must be new");return 2;}
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(OHMYDSP_SERVICE_CLASS));
    if(!service){fprintf(stderr,"no service\n");return 1;}
    CFTypeRef ready=IORegistryEntryCreateCFProperty(service,CFSTR("ipc4-session-ready"),kCFAllocatorDefault,0);
    if(!ready || !CFEqual(ready,kCFBooleanTrue)){fprintf(stderr,"IPC4 session is not ready\n");if(ready)CFRelease(ready);IOObjectRelease(service);return 1;} CFRelease(ready);
    io_connect_t conn=0; kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&conn);IOObjectRelease(service);
    if(kr){fprintf(stderr,"open failed 0x%x (root required; stop daemon/test clients)\n",kr);return 1;}
    mach_vm_address_t ra=0,sa=0;mach_vm_size_t rs=0,ss=0;int result=1;
    if(IOConnectMapMemory64(conn,OHMYDSP_MEM_RING,mach_task_self(),&ra,&rs,kIOMapAnywhere|kIOMapReadOnly) || IOConnectMapMemory64(conn,OHMYDSP_MEM_STATUS,mach_task_self(),&sa,&ss,kIOMapAnywhere|kIOMapReadOnly))goto done;
    const volatile struct OhMyDSPStatus *s=(void*)sa; const uint8_t *ring=(void*)ra;
    if(ss<sizeof(*s)||s->magic!=OHMYDSP_STATUS_MAGIC||s->version!=OHMYDSP_STATUS_VERSION||s->frameBytes!=8||s->rate!=48000||s->channels!=2||!s->ringBytes||s->ringBytes>rs||s->ringBytes%8)goto done;
    for(long cycle=1;cycle<=cycles;cycle++) {
        size_t capacity=(duration+2)*384000,used=0;uint8_t *pcm=malloc(capacity);if(!pcm)goto done;
        int ok=1;struct position first,current,after;
        kr=IOConnectCallScalarMethod(conn,OHMYDSP_SEL_START,NULL,0,NULL,NULL);
        printf("cycle %ld START 0x%x\n",cycle,kr);fflush(stdout);
        if(kr){free(pcm);goto done;}
        if(!snapshot(s,&first)||first.state!=OHMYDSP_STATE_RUNNING){ok=0;goto stop;}
        uint64_t consumed=first.frames, begin=mach_absolute_time();
        while(seconds(mach_absolute_time()-begin)<duration) {
            usleep(3000);
            if(!snapshot(s,&current)||current.state!=OHMYDSP_STATE_RUNNING||current.epoch!=first.epoch||current.faults!=first.faults||current.frames<consumed||current.frames-consumed>=s->ringBytes/8){ok=0;break;}
            size_t count=(current.frames-consumed)*8;
            if(count>capacity-used){ok=0;break;}
            uint32_t pos=(consumed*8)%s->ringBytes;
            size_t tail=count<s->ringBytes-pos?count:s->ringBytes-pos;
            memcpy(pcm+used,ring+pos,tail);memcpy(pcm+used+tail,ring,count-tail);
            if(!snapshot(s,&after)||after.epoch!=first.epoch||after.frames<current.frames||after.frames-consumed>=s->ringBytes/8){ok=0;break;}
            used+=count;consumed=current.frames;
        }
        double elapsed=seconds(mach_absolute_time()-begin),ratio=(used/8.0)/(elapsed*48000);
        printf("cycle %ld frames=%zu elapsed=%.3f rate-ratio=%.5f continuity=%s\n",cycle,used/8,elapsed,ratio,ok?"PASS":"FAIL");
        if(ratio<0.90||ratio>1.10)ok=0;
stop:
        kr=IOConnectCallScalarMethod(conn,OHMYDSP_SEL_STOP,NULL,0,NULL,NULL);
        if(kr||!snapshot(s,&after)||after.state!=OHMYDSP_STATE_STOPPED)ok=0;
        char path[4096];snprintf(path,sizeof(path),"%s/cycle-%ld%s.wav",argv[3],cycle,ok?"":"-FAILED");
        if(!wav(path,pcm,(uint32_t)used))ok=0;free(pcm);
        printf("cycle %ld STOP 0x%x result=%s\n",cycle,kr,ok?"PASS":"FAIL");fflush(stdout);
        if(!ok)goto done;
    }
    result=0;
done:
    IOConnectCallScalarMethod(conn,OHMYDSP_SEL_STOP,NULL,0,NULL,NULL);
    if(ra)IOConnectUnmapMemory64(conn,OHMYDSP_MEM_RING,mach_task_self(),ra);
    if(sa)IOConnectUnmapMemory64(conn,OHMYDSP_MEM_STATUS,mach_task_self(),sa);
    IOServiceClose(conn);return result;
}
