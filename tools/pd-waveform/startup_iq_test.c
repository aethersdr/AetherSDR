#include "pd_local_iq.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
static int calls,packets,interrupts,failure;
static ssize_t receive(int fd,void* bytes,size_t count,int flags)
{
    (void)bytes;assert(fd==9 && count==1 && flags==(MSG_DONTWAIT|MSG_TRUNC));++calls;
    if(interrupts){--interrupts;errno=EINTR;return -1;}
    if(failure){errno=EIO;return -1;}
    if(packets){--packets;return 1472;}
    errno=EAGAIN;return -1;
}
static struct pd_local_iq fresh(void)
{
    struct pd_local_iq iq={.fd=9};assert(pd_guard_init(&iq.guard,0x20000001,1,48000,25000000));return iq;
}
int main(void)
{
    struct pd_local_iq iq=fresh(),before;memcpy(&before,&iq,sizeof(iq));
    calls=0;packets=81;assert(pd_local_iq_discard_startup_io(&iq,receive)==81 && calls==82 && !memcmp(&iq,&before,sizeof(iq)));
    calls=0;interrupts=1;packets=1;assert(pd_local_iq_discard_startup_io(&iq,receive)==1 && calls==3);
    calls=0;packets=3000;assert(pd_local_iq_discard_startup_io(&iq,receive)==-1 && errno==EOVERFLOW && calls==2048);packets=0;
    calls=0;failure=1;assert(pd_local_iq_discard_startup_io(&iq,receive)==-1 && errno==EIO && calls==1);failure=0;
    for(unsigned mode=0;mode<10;++mode){
        iq=fresh();switch(mode){case 0:iq.frames=1;break;case 1:iq.guard.seen=1;break;case 2:iq.last_receive_ns=1;break;case 3:iq.last_valid_ns=1;break;case 4:iq.stale=1;break;case 5:iq.invalid=1;break;case 6:iq.starts=1;break;case 7:iq.duplicates=1;break;case 8:iq.old=1;break;case 9:iq.foreign=1;break;}
        memcpy(&before,&iq,sizeof(iq));calls=0;assert(pd_local_iq_discard_startup_io(&iq,receive)==-1 && errno==EINVAL && !calls && !memcmp(&iq,&before,sizeof(iq)));
    }
    iq=fresh();assert(pd_local_iq_discard_startup_io(&iq,NULL)==-1);
    puts("PASS: bounded pre-API IQ discard, no state/fault erasure, interrupted reads, transport errors and active-stream refusal; no sockets");return 0;
}
