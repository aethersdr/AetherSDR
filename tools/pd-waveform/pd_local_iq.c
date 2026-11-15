#define _POSIX_C_SOURCE 200809L
#include "pd_local_iq.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

void pd_iq_put32(unsigned char *p,uint32_t v)
{
    p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;
}
void pd_iq_put64(unsigned char *p,uint64_t v)
{
    pd_iq_put32(p,v>>32);pd_iq_put32(p+4,(uint32_t)v);
}
uint32_t pd_iq_get32(const unsigned char *p)
{
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
uint64_t pd_iq_get64(const unsigned char *p)
{
    return (uint64_t)pd_iq_get32(p)<<32|pd_iq_get32(p+4);
}
int64_t pd_iq_monotonic_ns(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t)) { return -1; }
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}

int pd_local_iq_init(struct pd_local_iq *iq,int fd)
{
    if(!iq || fd<3) { return 0; }
    memset(iq,0,sizeof(*iq));iq->fd=-1;
    struct sockaddr_un address;
    socklen_t length=sizeof(address);
    int type=0;socklen_t size=sizeof(type);
    if(getsockname(fd,(struct sockaddr *)&address,&length) || address.sun_family!=AF_UNIX ||
       getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_DGRAM) { return 0; }
    struct pollfd poller={fd,POLLIN,0};
    if(poll(&poller,1,2000)!=1 || !(poller.revents&POLLIN)) { return 0; }
    unsigned char p[PD_IQ_CONFIG];
    struct iovec v={p,sizeof(p)};
    struct msghdr msg={.msg_iov=&v,.msg_iovlen=1};
    ssize_t n=recvmsg(fd,&msg,MSG_DONTWAIT);
    if(n!=PD_IQ_CONFIG || msg.msg_flags&MSG_TRUNC || memcmp(p,"PDIC",4) ||
       pd_iq_get32(p+4)!=1) { return 0; }
    uint64_t age=pd_iq_get64(p+24);
    if(age>1000000000 || !pd_guard_init(&iq->guard,pd_iq_get32(p+8),pd_iq_get64(p+16),
                                        pd_iq_get32(p+12),(int64_t)age)) { return 0; }
    unsigned char ready[16];
    memcpy(ready,"PDIR",4);pd_iq_put32(ready+4,1);pd_iq_put64(ready+8,iq->guard.generation);
    if(send(fd,ready,sizeof(ready),MSG_DONTWAIT)!=sizeof(ready)) { return 0; }
    iq->fd=fd;return 1;
}

int pd_local_iq_receive(struct pd_local_iq *iq,unsigned char *packet,size_t capacity,
                        size_t *payload_bytes)
{
    if(!iq || !packet || !payload_bytes || capacity<PD_IQ_MAX_PACKET) { errno=EINVAL;return -1; }
    *payload_bytes=0;
    unsigned char p[PD_IQ_HEADER+PD_IQ_MAX_PACKET];
    struct iovec v={p,sizeof(p)};
    struct msghdr msg={.msg_iov=&v,.msg_iovlen=1};
    ssize_t bytes=recvmsg(iq->fd,&msg,MSG_DONTWAIT);
    int64_t now=pd_iq_monotonic_ns();
    if(bytes<0) { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR ? 0 : -1; }
    if(!bytes || now<0) { return -1; }
    if(bytes<PD_IQ_HEADER || msg.msg_flags&MSG_TRUNC || memcmp(p,"PDIQ",4) ||
       pd_iq_get32(p+4)!=1 || pd_iq_get32(p+48)!=(size_t)bytes-PD_IQ_HEADER) {
        ++iq->invalid;iq->guard.reset_pending=1;return 2;
    }
    uint64_t generation=pd_iq_get64(p+8);
    if(generation!=iq->guard.generation) { ++iq->foreign;return 0; }
    int64_t kr=(int64_t)pd_iq_get64(p+16),hr=(int64_t)pd_iq_get64(p+24);
    int64_t hm=(int64_t)pd_iq_get64(p+32),fm=(int64_t)pd_iq_get64(p+40);
    if(kr<=0 || hr<kr || hm<0 || fm<hm || now<fm ||
       hr-kr>10000000000 || now-hm>10000000000 || pd_iq_get32(p+52)) {
        ++iq->invalid;iq->guard.reset_pending=1;return 2;
    }
    int64_t age=hr-kr+now-hm;
    iq->last_age_ns=age;
    if(age>iq->max_age_seen_ns) { iq->max_age_seen_ns=age; }
    iq->last_receive_ns=now;
    struct pd_guard_frame frame;
    enum pd_guard_result result=pd_guard_check(&iq->guard,generation,p+PD_IQ_HEADER,
                                              (size_t)bytes-PD_IQ_HEADER,age,&frame);
    if(result==PD_GUARD_STALE) { ++iq->stale;return 2; }
    if(result==PD_GUARD_INVALID) { ++iq->invalid;return 2; }
    if(result==PD_GUARD_DUPLICATE) { ++iq->duplicates;return 0; }
    if(result==PD_GUARD_OLD) { ++iq->old;return 0; }
    if(result==PD_GUARD_FOREIGN) { ++iq->foreign;return 0; }
    memcpy(packet,p+PD_IQ_HEADER,(size_t)bytes-PD_IQ_HEADER);
    *payload_bytes=(size_t)bytes-PD_IQ_HEADER;
    iq->last_valid_ns=now;
    ++iq->frames;
    if(result==PD_GUARD_START) { ++iq->starts;return 2; }
    return 1;
}

int pd_local_iq_discard_startup_io(struct pd_local_iq *iq,
    ssize_t (*receive)(int,void*,size_t,int))
{
    if(!iq || iq->fd<3 || !receive || !iq->guard.sid || !iq->guard.generation ||
       iq->frames || iq->guard.seen || iq->last_receive_ns || iq->last_valid_ns || iq->stale || iq->invalid ||
       iq->starts || iq->duplicates || iq->old || iq->foreign) {
        errno=EINVAL;return -1;
    }
    int discarded=0;
    for(unsigned attempt=0;attempt<2048;++attempt){
        unsigned char byte;ssize_t count=receive(iq->fd,&byte,1,MSG_DONTWAIT|MSG_TRUNC);
        if(count>=0){++discarded;continue;}
        if(errno==EAGAIN || errno==EWOULDBLOCK){return discarded;}
        if(errno!=EINTR){return -1;}
    }
    errno=EOVERFLOW;return -1;
}
int pd_local_iq_discard_startup(struct pd_local_iq *iq)
{
    return pd_local_iq_discard_startup_io(iq,recv);
}
