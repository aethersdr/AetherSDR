#define _GNU_SOURCE
#include "pd_start_gate.h"
#include "pd_owner.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static const unsigned char kReady[8]={'P','D','R','E','A','D','Y','1'};
static const unsigned char kRelease[8]={'P','D','R','U','N','0','0','1'};
enum { kStartupDeadlineMs=30000 };
int pd_start_gate_wait_io(int fd,int owner,const struct pd_start_gate_io* io)
{
    if(fd==-1){return 1;}
    if(fd<0){return -1;}
    if(owner<0 || !io || !io->now_ms || !io->owner_closed || !io->send_ready || !io->receive_release || !io->wait){return -1;}
    if(io->owner_closed(owner)){return 0;}
    int64_t start=io->now_ms();if(start<0){return -1;}
    if(io->send_ready(fd,kReady,sizeof(kReady))!=(ssize_t)sizeof(kReady)){return -1;}
    for(;;){
        if(io->owner_closed(owner)){return 0;}
        int64_t now=io->now_ms();if(now<start || now-start>=kStartupDeadlineMs){return -1;}
        int events=io->wait(fd,owner,100);
        if(events<0 || events&PD_START_GATE_ERROR){return -1;}
        if(events&PD_START_GATE_CANCELED || io->owner_closed(owner)){return 0;}
        now=io->now_ms();if(now<start || now-start>=kStartupDeadlineMs){return -1;}
        if(events&PD_START_GATE_READABLE){
            unsigned char bytes[9];ssize_t count=io->receive_release(fd,bytes,sizeof(bytes));
            if(count<0 && (errno==EAGAIN || errno==EINTR)){continue;}
            if(io->owner_closed(owner)){return 0;}
            return count==(ssize_t)sizeof(kRelease) && !memcmp(bytes,kRelease,sizeof(kRelease)) ? 1 : -1;
        }
    }
}
static int64_t time_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static ssize_t ready(int fd,const void* bytes,size_t count){return send(fd,bytes,count,MSG_DONTWAIT|MSG_NOSIGNAL);}
static ssize_t release(int fd,void* bytes,size_t count){return recv(fd,bytes,count,MSG_DONTWAIT|MSG_TRUNC);}
static int wait_gate(int fd,int owner,int milliseconds)
{
    struct pollfd p[2]={{fd,POLLIN,0},{owner,0,0}};int result=poll(p,2,milliseconds);
    if(result<0){return errno==EINTR ? 0 : -1;}
    if(p[1].revents&(POLLHUP|POLLERR|POLLNVAL)){return PD_START_GATE_CANCELED;}
    if(p[0].revents&(POLLHUP|POLLERR|POLLNVAL)){return PD_START_GATE_ERROR;}
    return p[0].revents&POLLIN ? PD_START_GATE_READABLE : 0;
}
int pd_start_gate_wait(int fd,int owner)
{
    const struct pd_start_gate_io io={time_ms,pd_owner_closed,ready,release,wait_gate};
    return pd_start_gate_wait_io(fd,owner,&io);
}
