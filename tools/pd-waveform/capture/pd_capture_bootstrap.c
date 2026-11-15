#define _GNU_SOURCE
#include "pd_ring_worker.c"
#include <arpa/inet.h>
#include <dirent.h>
#include <grp.h>
#include <limits.h>
#include <linux/filter.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/wait.h>

static volatile sig_atomic_t capture_stopping;
static void capture_signal(int number){if(number!=SIGCHLD){capture_stopping=1;}}
static int capture_number(const char* text,int base,unsigned digits,uint64_t* value)
{
    if(!text || !*text || strlen(text)>digits){return 0;}
    for(const char* p=text;*p;++p){
        if(*p>='0' && *p<='9'){continue;}
        if(base==16 && ((*p>='a' && *p<='f') || (*p>='A' && *p<='F'))){continue;}return 0;
    }
    return numeric(text,base,value);
}
static int capture_socket_type(int fd,int type)
{
    int actual=0;socklen_t size=sizeof(actual);return !getsockopt(fd,SOL_SOCKET,SO_TYPE,&actual,&size) && actual==type;
}
static int capture_endpoint(int fd,unsigned* port)
{
    struct sockaddr_in a={0};socklen_t size=sizeof(a);int protocol=0;socklen_t n=sizeof(protocol);struct in_addr expected;
    if(inet_pton(AF_INET,"172.30.2.1",&expected)!=1 || !capture_socket_type(fd,SOCK_DGRAM) ||
       getsockname(fd,(struct sockaddr*)&a,&size) || size!=sizeof(a) || a.sin_family!=AF_INET ||
       a.sin_addr.s_addr!=expected.s_addr || ntohs(a.sin_port)<1024 ||
       getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&protocol,&n) || protocol!=IPPROTO_UDP){return 0;}
    int reused=0;n=sizeof(reused);if(getsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reused,&n) || reused){return 0;}
    n=sizeof(reused);
    if(getsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reused,&n)){if(errno!=ENOPROTOOPT){return 0;}}
    else if(reused){return 0;}
    *port=ntohs(a.sin_port);return 1;
}
static int capture_private_output(int fd)
{
    struct sockaddr_un a={0};socklen_t size=sizeof(a);
    if(!capture_socket_type(fd,SOCK_DGRAM) || getsockname(fd,(struct sockaddr*)&a,&size) || a.sun_family!=AF_UNIX || size!=offsetof(struct sockaddr_un,sun_path)){return 0;}
    size=sizeof(a);return !getpeername(fd,(struct sockaddr*)&a,&size) && a.sun_family==AF_UNIX && size==offsetof(struct sockaddr_un,sun_path);
}
static int capture_inventory(void)
{
    int control=socket(AF_INET,SOCK_DGRAM,0);if(control<0){return 0;}
    struct ifreq r={0};memcpy(r.ifr_name,"eth0.10",8);int index=0;struct in_addr expected;
    if(inet_pton(AF_INET,"172.30.2.1",&expected)!=1 || ioctl(control,SIOCGIFADDR,&r) || r.ifr_addr.sa_family!=AF_INET ||
       ((struct sockaddr_in*)&r.ifr_addr)->sin_addr.s_addr!=expected.s_addr || ioctl(control,SIOCGIFFLAGS,&r) || !(r.ifr_flags&IFF_UP) ||
       ioctl(control,SIOCGIFHWADDR,&r) || r.ifr_hwaddr.sa_family!=ARPHRD_ETHER){close(control);return 0;}
    unsigned char* mac=(unsigned char*)r.ifr_hwaddr.sa_data;unsigned nonzero=0;
    for(unsigned i=0;i<6;++i){nonzero|=mac[i];}if(!nonzero || mac[0]&1){close(control);return 0;}
    if(!ioctl(control,SIOCGIFINDEX,&r) && r.ifr_ifindex>0){index=r.ifr_ifindex;}close(control);return index;
}
static int capture_close_others(int udp,int output,int life)
{
    DIR* d=opendir("/proc/self/fd");if(!d){return 0;}int directory_fd=dirfd(d);struct dirent* e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'){continue;}char* end;errno=0;long fd=strtol(e->d_name,&end,10);
        if(errno || *end || fd<0 || fd>INT_MAX){closedir(d);return 0;}
        if(fd>2 && fd!=udp && fd!=output && fd!=life && fd!=directory_fd){close((int)fd);}
    }
    return !closedir(d);
}
static int capture_cap_mask(unsigned mask)
{
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    caps[0].effective=caps[0].permitted=mask;
    if(syscall(SYS_capset,&h,caps) || prctl(PR_SET_DUMPABLE,0,0,0,0)){return 0;}
    memset(caps,0,sizeof(caps));return !syscall(SYS_capget,&h,caps) && caps[0].effective==mask && caps[0].permitted==mask &&
        !caps[0].inheritable && !caps[1].effective && !caps[1].permitted && !caps[1].inheritable;
}
static int capture_drop_root(void)
{
    for(int cap=0;cap<64;++cap){if(prctl(PR_CAPBSET_DROP,cap,0,0,0) && errno!=EINVAL){return 0;}}
    return !prctl(PR_SET_KEEPCAPS,1,0,0,0) && !setgroups(0,NULL) && !setresgid(65534,65534,65534) && !setresuid(65534,65534,65534) &&
        capture_cap_mask(1u<<CAP_NET_RAW) && !prctl(PR_SET_KEEPCAPS,0,0,0,0) && geteuid()==65534;
}
static unsigned capture_filter(struct sock_filter code[48],unsigned port,uint32_t sid,unsigned rate)
{
    unsigned cls=rate==24000?0x02e3:rate==48000?0x02e4:rate==96000?0x02e5:0x02e6;
    struct sock_filter values[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_LEN,0),BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,56,0,0),
        BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,PD_IQ_MAX_PACKET+68,0,0),
        BPF_STMT(BPF_LD|BPF_B|BPF_ABS,0),BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xf0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x40,0,0),
        BPF_STMT(BPF_LD|BPF_B|BPF_ABS,0),BPF_STMT(BPF_ALU|BPF_AND|BPF_K,15),BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,5,0,0),
        BPF_STMT(BPF_LD|BPF_B|BPF_ABS,9),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,17,0,0),
        BPF_STMT(BPF_LD|BPF_H|BPF_ABS,6),BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0x3fff),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,12),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0xc0a83264,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,16),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0xac1e0201,0,0),
        BPF_STMT(BPF_LDX|BPF_B|BPF_MSH,0),BPF_STMT(BPF_LD|BPF_H|BPF_IND,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,50001,0,0),
        BPF_STMT(BPF_LD|BPF_H|BPF_IND,2),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,port,0,0),
        BPF_STMT(BPF_LD|BPF_H|BPF_IND,4),BPF_JUMP(BPF_JMP|BPF_JGE|BPF_K,36,0,0),BPF_JUMP(BPF_JMP|BPF_JGT|BPF_K,PD_IQ_MAX_PACKET+8,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_IND,8),BPF_STMT(BPF_ALU|BPF_AND|BPF_K,0xf8f00000),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x18600000,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_IND,12),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,sid,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_IND,16),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x1c2d,0,0),
        BPF_STMT(BPF_LD|BPF_W|BPF_IND,20),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,0x534c0000|cls,0,0),
        BPF_STMT(BPF_RET|BPF_K,65535),BPF_STMT(BPF_RET|BPF_K,0)
    };
    unsigned count=sizeof(values)/sizeof(values[0]);memcpy(code,values,sizeof(values));
    for(unsigned i=0;i<count;++i){
        if(BPF_CLASS(code[i].code)!=BPF_JMP){continue;}
        if(BPF_OP(code[i].code)==BPF_JGT){code[i].jt=(unsigned char)(count-2-i);}
        else{code[i].jf=(unsigned char)(count-2-i);}
    }
    return count;
}
static int capture_open(int index,unsigned port,uint32_t sid,unsigned rate)
{
    int fd=socket(AF_PACKET,SOCK_DGRAM,0);if(fd<0){return -1;}
    struct sock_filter code[48];unsigned count=capture_filter(code,port,sid,rate);struct sock_fprog program={(unsigned short)count,code};int bytes=1<<20;
    struct sockaddr_ll a={.sll_family=AF_PACKET,.sll_protocol=htons(ETH_P_IP),.sll_ifindex=index};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes)) || setsockopt(fd,SOL_SOCKET,SO_ATTACH_FILTER,&program,sizeof(program)) ||
       bind(fd,(struct sockaddr*)&a,sizeof(a))){close(fd);return -1;}
    return fd;
}
static void capture_event(const char* name,pid_t worker,int status)
{
    char out[320];int n=snprintf(out,sizeof(out),"{\"event\":\"%s\",\"uid\":%lu,\"worker_pid\":%ld,\"status\":%d,\"rf_commands\":[]}\n",name,(unsigned long)geteuid(),(long)worker,status);
    if(n>0 && n<(int)sizeof(out)){(void)write(1,out,(size_t)n);}
}
int main(int argc,char** argv)
{
    if(argc!=8 || geteuid()!=0){return 2;}
    uint64_t v[7];for(unsigned i=0;i<7;++i){int base=i==3||i==4?16:10;unsigned digits=i==3?8:i==4?16:6;if(!capture_number(argv[i+1],base,digits,&v[i])){return 2;}}
    int udp=(int)v[0],output=(int)v[1],life=(int)v[2];
    for(unsigned i=0;i<3;++i){if(v[i]<3 || v[i]>1023){return 2;}}
    if(udp==output || udp==life || output==life || !v[3] || v[3]>UINT32_MAX || !v[4] ||
       (v[5]!=24000 && v[5]!=48000 && v[5]!=96000 && v[5]!=192000) || v[6]>120){return 2;}
    struct stat st;unsigned port=0;
    if(!capture_endpoint(udp,&port) || !capture_private_output(output) || fstat(life,&st) || !S_ISFIFO(st.st_mode) ||
       (fcntl(life,F_GETFL)&O_ACCMODE)!=O_RDONLY || fstat(1,&st) || !S_ISFIFO(st.st_mode)){return 3;}
    int flags=fcntl(1,F_GETFL);if(flags<0 || fcntl(1,F_SETFL,flags|O_NONBLOCK) || dup2(1,0)<0 || dup2(1,2)<0){return 3;}
    if(!capture_close_others(udp,output,life)){return 3;}
    int index=capture_inventory();if(!index || !capture_drop_root()){capture_event("constructor_rejected",0,errno);return 4;}
    int packet=capture_open(index,port,(uint32_t)v[3],(unsigned)v[5]);
    if(packet<0 || !capture_cap_mask(0)){capture_event("capture_setup_failed",0,errno);return 5;}
    struct sigaction action={.sa_handler=capture_signal};sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL) || sigaction(SIGCHLD,&action,NULL) || signal(SIGPIPE,SIG_IGN)==SIG_ERR){return 5;}
    pid_t parent=getpid(),worker=fork();if(worker<0){return 6;}
    if(!worker){
        if(prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0) || getppid()!=parent){_exit(6);}
        close(life);close(udp);struct sigaction normal={.sa_handler=SIG_DFL};sigemptyset(&normal.sa_mask);
        if(sigaction(SIGTERM,&normal,NULL) || sigaction(SIGINT,&normal,NULL)){_exit(6);}
        char capture_text[16],output_text[16];snprintf(capture_text,sizeof(capture_text),"%d",packet);snprintf(output_text,sizeof(output_text),"%d",output);
        char* args[]={argv[0],argv[4],argv[5],argv[6],argv[7],capture_text,output_text,NULL};
        _exit(pd_ring_worker_main(7,args,1));
    }
    close(packet);close(output);close(0);close(2);
    capture_event("supervisor_ready",worker,0);int status=0,intentional=0;
    for(;;){
        pid_t got=waitpid(worker,&status,WNOHANG);if(got==worker){break;}if(got<0 && errno!=EINTR){return 7;}
        struct pollfd p={life,0,0};int ready=poll(&p,1,500);
        if(capture_stopping || (ready>0 && p.revents&(POLLHUP|POLLERR|POLLNVAL)) || (ready<0 && errno!=EINTR)){
            intentional=1;kill(worker,SIGKILL);while(waitpid(worker,&status,0)<0){if(errno!=EINTR){return 7;}}break;
        }
    }
    close(life);close(udp);capture_event(intentional?"session_stopped":"worker_stopped",worker,status);
    return intentional?0:WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
