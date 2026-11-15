#include "pd_start_gate.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int64_t clock_ms;
static int mode,sends,waits,reads,checks;
static int64_t now(void){clock_ms+=1000;return mode==8 && waits ? -1 : clock_ms;}
static int closed(int fd){assert(fd==6);++checks;return mode==1 || (mode==2 && waits) || (mode==9 && waits) || (mode==11 && reads);}
static ssize_t send_ready(int fd,const void* bytes,size_t size)
{
    assert(fd==9 && size==8 && !memcmp(bytes,"PDREADY1",8));++sends;return mode==3 ? 7 : 8;
}
static ssize_t receive_release(int fd,void* bytes,size_t size)
{
    assert(fd==9 && size==9);++reads;
    if(mode==5){memcpy(bytes,"PDRUN001X",9);return 9;}
    if(mode==6){memcpy(bytes,"PDRUN000",8);return 8;}
    if(mode==7 && reads==1){errno=EAGAIN;return -1;}
    memcpy(bytes,"PDRUN001",8);return 8;
}
static int wait_gate(int fd,int owner,int milliseconds)
{
    assert(fd==9 && owner==6 && milliseconds==100);++waits;
    if(mode==4 || mode==8){return 0;}if(mode==10){return PD_START_GATE_ERROR;}
    if(mode==2){return PD_START_GATE_CANCELED;}
    if(mode==12){clock_ms=31000;}
    return PD_START_GATE_READABLE;
}
static int run(int which)
{
    mode=which;sends=waits=reads=checks=0;clock_ms=0;
    const struct pd_start_gate_io io={now,closed,send_ready,receive_release,wait_gate};
    return pd_start_gate_wait_io(9,6,&io);
}
int main(void)
{
    assert(pd_start_gate_wait(-1,-1)==1);
    assert(pd_start_gate_wait(-2,6)==-1);
    assert(pd_start_gate_wait_io(9,-1,NULL)==-1);
    assert(run(0)==1 && sends==1 && reads==1);
    assert(run(1)==0 && !sends && !reads);
    assert(run(2)==0 && sends==1 && !reads);
    assert(run(3)==-1 && sends==1 && !reads);
    assert(run(4)==-1 && sends==1 && waits>1 && waits<30 && !reads);
    assert(run(5)==-1 && reads==1);
    assert(run(6)==-1 && reads==1);
    assert(run(7)==1 && reads==2 && sends==1);
    assert(run(8)==-1 && !reads);
    assert(run(9)==0 && !reads);
    assert(run(10)==-1 && !reads);
    assert(run(11)==0 && reads==1);
    assert(run(12)==-1 && !reads);
    puts("PASS: private pre-API startup barrier release, owner cancellation, malformed/oversized input, backpressure, retry, timeout and clock failure; socket-free");
    return 0;
}
