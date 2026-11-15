#define _GNU_SOURCE
#include "pd_local_iq.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Trusted launcher supplies a fixed filtered packet FD and private Unix
 * datagram endpoint. No untrusted packet is parsed before the packet FD is
 * closed and strict seccomp is entered. The clock child closes the packet
 * FD before the receive ring is mapped, so it never inherits that mapping. */
enum { kFrameBytes=8192,kFrames=64,kRingBytes=kFrameBytes*kFrames };
struct ring_clock { uint32_t sequence,words[6]; };
struct clock_sample { uint32_t sequence;int64_t real,low,high; };
struct ring_state {
    int output;uint32_t sid;uint64_t generation;
    uint64_t received,forwarded,stale,invalid,foreign,backpressure,output_errors,loss_seen,clock_faults,future_wait;
    int64_t previous_offset_low,previous_offset_high;int have_offset;
};
static _Noreturn void raw_exit(int code) { syscall(SYS_exit,code);for(;;){} }
static int raw_write(int fd,const void* data,size_t n)
{
    long rc=syscall(SYS_write,fd,data,n);
    return rc==(long)n;
}
static int64_t clock_ns(clockid_t id)
{
    struct timespec t;if(clock_gettime(id,&t)){return -1;}
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void clock_store(struct ring_clock* page,int64_t real,int64_t low,int64_t high)
{
    uint32_t next=__atomic_load_n(&page->sequence,__ATOMIC_RELAXED)+1;
    __atomic_store_n(&page->sequence,next,__ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t values[3]={(uint64_t)real,(uint64_t)low,(uint64_t)high};
    for(unsigned i=0;i<3;++i){
        __atomic_store_n(&page->words[2*i],(uint32_t)values[i],__ATOMIC_RELAXED);
        __atomic_store_n(&page->words[2*i+1],(uint32_t)(values[i]>>32),__ATOMIC_RELAXED);
    }
    __atomic_store_n(&page->sequence,next+1,__ATOMIC_RELEASE);
}
static int clock_load(const struct ring_clock* page,struct clock_sample* sample)
{
    for(unsigned attempt=0;attempt<16;++attempt){
        uint32_t before=__atomic_load_n(&page->sequence,__ATOMIC_ACQUIRE);
        if(!before || before&1){continue;}
        uint64_t values[3];
        for(unsigned i=0;i<3;++i){
            uint32_t low=__atomic_load_n(&page->words[2*i],__ATOMIC_RELAXED);
            uint32_t high=__atomic_load_n(&page->words[2*i+1],__ATOMIC_RELAXED);
            values[i]=(uint64_t)high<<32|low;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if(before!=__atomic_load_n(&page->sequence,__ATOMIC_RELAXED)){continue;}
        *sample=(struct clock_sample){before,(int64_t)values[0],(int64_t)values[1],(int64_t)values[2]};
        return sample->real>0 && sample->real<INT64_MAX/2 && sample->low>0 &&
               sample->low<INT64_MAX/2 && sample->high>=sample->low && sample->high-sample->low<=1000000;
    }
    return 0;
}
static _Noreturn void clock_process(struct ring_clock* page,int control,int capture,int output,int other,pid_t parent,int seconds)
{
    close(capture);close(output);close(other);close(0);close(1);close(2);
    if(prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0) || getppid()!=parent){raw_exit(2);}
    (void)setpriority(PRIO_PROCESS,0,10);
    int64_t start=clock_ns(CLOCK_MONOTONIC);if(start<=0){raw_exit(3);}
    for(;;){
        int64_t low=clock_ns(CLOCK_MONOTONIC),real=clock_ns(CLOCK_REALTIME),high=clock_ns(CLOCK_MONOTONIC);
        if(low<=0 || real<=0 || high<low || (seconds>0 && high-start>(int64_t)(seconds+5)*1000000000)){break;}
        clock_store(page,real,low,high);
        struct pollfd p={control,POLLIN,0};
        int rc=poll(&p,1,1);if(rc>0 || (rc<0 && errno!=EINTR)){break;}
    }
    raw_exit(0);
}
static int clock_valid(struct ring_state* state,const struct clock_sample* sample)
{
    int64_t low=sample->real-sample->high,high=sample->real-sample->low;
    if(state->have_offset && (low>state->previous_offset_high+1000000 || high<state->previous_offset_low-1000000)){
        ++state->clock_faults;return 0;
    }
    state->have_offset=1;state->previous_offset_low=low;state->previous_offset_high=high;return 1;
}
static void envelope(unsigned char* out,const struct ring_state* state,const struct clock_sample* sample,int64_t kernel,unsigned bytes,unsigned flags)
{
    memcpy(out,"PDIQ",4);pd_iq_put32(out+4,1);pd_iq_put64(out+8,state->generation);
    pd_iq_put64(out+16,(uint64_t)kernel);pd_iq_put64(out+24,(uint64_t)sample->real);
    pd_iq_put64(out+32,(uint64_t)sample->low);pd_iq_put64(out+40,(uint64_t)sample->low);
    pd_iq_put32(out+48,bytes);pd_iq_put32(out+52,flags);
}
static void reset_notice(struct ring_state* state,const struct clock_sample* sample)
{
    unsigned char out[PD_IQ_HEADER];envelope(out,state,sample,sample->real,0,1);
    if(!raw_write(state->output,out,sizeof(out)) && errno!=EAGAIN && errno!=EWOULDBLOCK){++state->output_errors;}
}
/* 0: consumed frame; 1: newer than clock, retain for next tick; -1: output failed. */
static int forward_frame(struct ring_state* state,const struct clock_sample* sample,struct tpacket2_hdr* frame)
{
    unsigned flags=__atomic_load_n(&frame->tp_status,__ATOMIC_ACQUIRE);
    int64_t kernel=(int64_t)frame->tp_sec*1000000000+frame->tp_nsec;
    if(frame->tp_sec && frame->tp_nsec<1000000000 && kernel>sample->real){
        if(kernel-sample->real>25000000){++state->received;++state->invalid;reset_notice(state,sample);return 0;}
        ++state->future_wait;return 1;
    }
    ++state->received;
    if(!frame->tp_sec || frame->tp_nsec>=1000000000 || frame->tp_snaplen!=frame->tp_len ||
       frame->tp_snaplen<48 || frame->tp_snaplen>PD_IQ_MAX_PACKET+68 || frame->tp_net<TPACKET2_HDRLEN ||
       frame->tp_net>kFrameBytes || frame->tp_snaplen>kFrameBytes-frame->tp_net){
        ++state->invalid;reset_notice(state,sample);return 0;
    }
    if(flags&TP_STATUS_LOSING && !state->loss_seen){state->loss_seen=1;reset_notice(state,sample);}
    if(sample->real-kernel>25000000){++state->stale;return 0;}
    const unsigned char* packet=(unsigned char*)frame+frame->tp_net;
    unsigned ihl=(packet[0]&15)*4;
    unsigned total=(unsigned)packet[2]<<8|packet[3];
    if(packet[0]>>4!=4 || ihl<20 || ihl>60 || frame->tp_snaplen<ihl+8 || packet[9]!=17 ||
       (((unsigned)packet[6]<<8|packet[7])&0x3fff) || total<ihl+8 || total>frame->tp_snaplen){
        ++state->invalid;reset_notice(state,sample);return 0;
    }
    unsigned udp=(unsigned)packet[ihl+4]<<8|packet[ihl+5];
    if(udp<36 || udp!=total-ihl || udp-8>PD_IQ_MAX_PACKET){++state->invalid;reset_notice(state,sample);return 0;}
    if(pd_iq_get32(packet+ihl+12)!=state->sid){++state->foreign;return 0;}
    unsigned char out[PD_IQ_HEADER+PD_IQ_MAX_PACKET];envelope(out,state,sample,kernel,udp-8,0);
    memcpy(out+PD_IQ_HEADER,packet+ihl+8,udp-8);
    if(!raw_write(state->output,out,PD_IQ_HEADER+udp-8)){
        if(errno==EAGAIN || errno==EWOULDBLOCK){++state->backpressure;return 0;}
        ++state->output_errors;return -1;
    }
    ++state->forwarded;return 0;
}
static unsigned discard_startup(unsigned char* ring,uint64_t* discarded)
{
    unsigned cursor=0;
    for(unsigned i=0;i<kFrames;++i){
        struct tpacket2_hdr* frame=(struct tpacket2_hdr*)(ring+cursor*kFrameBytes);
        if(!(__atomic_load_n(&frame->tp_status,__ATOMIC_ACQUIRE)&TP_STATUS_USER)){break;}
        __atomic_store_n(&frame->tp_status,TP_STATUS_KERNEL,__ATOMIC_RELEASE);
        ++*discarded;cursor=(cursor+1)%kFrames;
    }
    return cursor;
}
static int numeric(const char* text,int base,uint64_t* out)
{
    if(!text || !*text || *text=='-' || *text=='+'){return 0;}
    char* end;errno=0;unsigned long long n=strtoull(text,&end,base);
    if(errno || *end){return 0;}*out=n;return 1;
}
static int pd_ring_worker_main(int argc,char** argv,int lifetime_controlled)
{
    uint64_t sid,generation,rate,seconds,capture_fd,output_fd;
    if(argc!=7 || !numeric(argv[1],16,&sid) || !sid || sid>UINT32_MAX || !numeric(argv[2],16,&generation) || !generation ||
       !numeric(argv[3],10,&rate) || (rate!=24000 && rate!=48000 && rate!=96000 && rate!=192000) ||
       !numeric(argv[4],10,&seconds) || (!seconds && !lifetime_controlled) || seconds>120 || !numeric(argv[5],10,&capture_fd) || capture_fd<3 || capture_fd>1023 ||
       !numeric(argv[6],10,&output_fd) || output_fd<3 || output_fd>1023 || capture_fd==output_fd){return 2;}
    int capture=(int)capture_fd,output=(int)output_fd;
    pid_t launcher=getppid();if(launcher<=1 || prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0) || getppid()!=launcher){return 3;}
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    if(geteuid()==0 || syscall(SYS_capget,&h,caps) || caps[0].effective || caps[0].permitted || caps[1].effective || caps[1].permitted ||
       prctl(PR_SET_DUMPABLE,0,0,0,0)){return 3;}
    struct sockaddr_storage address;socklen_t length=sizeof(address);int type=0;socklen_t size=sizeof(type);
    if(getsockname(capture,(struct sockaddr*)&address,&length) || address.ss_family!=AF_PACKET ||
       getsockopt(capture,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_DGRAM){return 4;}
    length=sizeof(address);size=sizeof(type);
    if(getpeername(output,(struct sockaddr*)&address,&length) || address.ss_family!=AF_UNIX ||
       getsockopt(output,SOL_SOCKET,SO_TYPE,&type,&size) || type!=SOCK_DGRAM){return 4;}
    struct stat st;if(fstat(1,&st) || !S_ISFIFO(st.st_mode)){return 4;}
    /* Reserve stdio slots until internal pipes exist; daemons may inherit
     * closed stdin/stderr. Neither inherited descriptor is a worker input. */
    if(dup2(1,0)<0 || dup2(1,2)<0){return 4;}
    int output_flags=fcntl(output,F_GETFL);if(output_flags<0 || fcntl(output,F_SETFL,output_flags|O_NONBLOCK)){return 4;}
    struct ring_clock* page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(page==MAP_FAILED){return 5;}
    int control[2];if(pipe(control)){return 5;}
    struct sigaction action={.sa_handler=SIG_IGN};sigemptyset(&action.sa_mask);if(sigaction(SIGCHLD,&action,NULL) || sigaction(SIGPIPE,&action,NULL)){return 5;}
    pid_t parent=getpid(),ticker=fork();if(ticker<0){return 5;}
    if(!ticker){clock_process(page,control[0],capture,output,control[1],parent,(int)seconds);}
    close(control[0]);
    int version=TPACKET_V2;struct tpacket_req req={.tp_block_size=8192,.tp_block_nr=64,.tp_frame_size=8192,.tp_frame_nr=64};
    if(setsockopt(capture,SOL_PACKET,PACKET_VERSION,&version,sizeof(version)) || setsockopt(capture,SOL_PACKET,PACKET_RX_RING,&req,sizeof(req))){return 5;}
    unsigned char* ring=mmap(NULL,kRingBytes,PROT_READ|PROT_WRITE,MAP_SHARED,capture,0);if(ring==MAP_FAILED){return 5;}
    if(close(capture) || fcntl(capture,F_GETFD)!=-1 || errno!=EBADF || mprotect(page,4096,PROT_READ)){return 5;}
    close(0);close(2);
    int timer=timerfd_create(CLOCK_MONOTONIC,0);struct itimerspec period={{0,1000000},{0,1000000}};
    if(timer<0 || timerfd_settime(timer,0,&period,NULL)){return 5;}
    unsigned char config[PD_IQ_CONFIG]={0};memcpy(config,"PDIC",4);pd_iq_put32(config+4,1);pd_iq_put32(config+8,(uint32_t)sid);
    pd_iq_put32(config+12,(uint32_t)rate);pd_iq_put64(config+16,generation);pd_iq_put64(config+24,25000000);
    if(!raw_write(output,config,sizeof(config))){return 6;}
    struct pollfd ready_poll={output,POLLIN,0};unsigned char ready[17];
    if(poll(&ready_poll,1,3000)!=1 || !(ready_poll.revents&POLLIN) || recv(output,ready,sizeof(ready),MSG_DONTWAIT)!=16 ||
       memcmp(ready,"PDIR",4) || pd_iq_get32(ready+4)!=1 || pd_iq_get64(ready+8)!=generation){return 6;}
    uint64_t startup_discarded=0;unsigned cursor=discard_startup(ring,&startup_discarded);
    char report[1536];int n=snprintf(report,sizeof(report),"{\"event\":\"ready\",\"uid\":%lu,\"clock_pid\":%ld,\"raw_fd_closed\":true,\"strict_seccomp\":true}\n",(unsigned long)geteuid(),(long)ticker);
    if(prctl(PR_SET_SECCOMP,1,0,0,0)){return 7;}
    if(n<=0 || n>=(int)sizeof(report) || !raw_write(1,report,(size_t)n)){raw_exit(7);}
    struct ring_state state={.output=output,.sid=(uint32_t)sid,.generation=generation};
    struct clock_sample sample={0};uint32_t previous_sequence=0;uint64_t stopped_ticks=0;int64_t start=0;const char* reason="duration";
    for(;;){
        uint64_t ticks=0;long got=syscall(SYS_read,timer,&ticks,sizeof(ticks));
        if(got<0 && errno==EINTR){continue;}
        if(got!=sizeof(ticks) || !ticks){reason="timer";break;}
        if(!clock_load(page,&sample)){if(ticks>25 || stopped_ticks>25-ticks){reason="clock_unavailable";break;}stopped_ticks+=ticks;continue;}
        if(sample.sequence==previous_sequence){if(ticks>25 || stopped_ticks>25-ticks){reason="clock_stalled";break;}stopped_ticks+=ticks;}
        else{stopped_ticks=0;previous_sequence=sample.sequence;}
        if(!clock_valid(&state,&sample)){reset_notice(&state,&sample);reason="clock_step";break;}
        if(!start){start=sample.low;}
        if(sample.low<start || (seconds && sample.low-start>=(int64_t)seconds*1000000000)){break;}
        int failed=0;
        for(unsigned batch=0;batch<kFrames;++batch){
            struct tpacket2_hdr* frame=(struct tpacket2_hdr*)(ring+cursor*kFrameBytes);
            if(!(__atomic_load_n(&frame->tp_status,__ATOMIC_ACQUIRE)&TP_STATUS_USER)){break;}
            int result=forward_frame(&state,&sample,frame);if(result==1){break;}
            __atomic_store_n(&frame->tp_status,TP_STATUS_KERNEL,__ATOMIC_RELEASE);cursor=(cursor+1)%kFrames;
            if(result<0){failed=1;break;}
        }
        if(failed){reason="output";break;}
    }
    n=snprintf(report,sizeof(report),"{\"event\":\"complete\",\"reason\":\"%s\",\"received\":%llu,\"forwarded\":%llu,\"stale\":%llu,\"invalid\":%llu,\"foreign\":%llu,\"backpressure\":%llu,\"output_errors\":%llu,\"loss_seen\":%llu,\"clock_faults\":%llu,\"future_wait\":%llu,\"clock_pid\":%ld,\"startup_discarded\":%llu,\"strict_seccomp\":true,\"raw_fd_closed\":true,\"rf_commands\":[]}\n",reason,
      (unsigned long long)state.received,(unsigned long long)state.forwarded,(unsigned long long)state.stale,(unsigned long long)state.invalid,(unsigned long long)state.foreign,
      (unsigned long long)state.backpressure,(unsigned long long)state.output_errors,(unsigned long long)state.loss_seen,(unsigned long long)state.clock_faults,(unsigned long long)state.future_wait,(long)ticker,(unsigned long long)startup_discarded);
    if(n>0 && n<(int)sizeof(report)){(void)raw_write(1,report,(size_t)n);}
    (void)raw_write(control[1],"E",1);raw_exit(state.forwarded && (!strcmp(reason,"duration") || !strcmp(reason,"output"))?0:8);
}
