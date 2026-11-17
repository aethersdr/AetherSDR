#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <netinet/in.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int pd_case_recovery_collection(void);
int pd_case_eq_phase_startup(void);
int pd_case_warm_core(void);
int pd_case_eq_worker(void);
int pd_case_dispatch_deadline(void);
enum { kCases=5,kOutput=1024,kReport=16384,kPort=48999 };
static const char* const names[kCases]={"recovery_collection","eq_phase_startup","warm_core","eq_worker","dispatch_deadline"};
static int (*const cases[kCases])(void)={pd_case_recovery_collection,pd_case_eq_phase_startup,pd_case_warm_core,pd_case_eq_worker,pd_case_dispatch_deadline};
static volatile sig_atomic_t stopping;
static void stop_probe(int number){(void)number;stopping=1;}
static uint64_t monotonic_ns(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t) || t.tv_sec<0){return 0;}
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
struct result {
    int passed,exit_code,signal_number,timed_out,error,truncated;
    uint64_t elapsed_ns,user_us,system_us;
    long max_rss_kib;
    char output[kOutput];size_t used;
};
static void drain(int fd,struct result* result)
{
    char bytes[512];ssize_t got;
    for(unsigned batch=0;batch<8 && (got=read(fd,bytes,sizeof(bytes)))>0;++batch){
        size_t keep=(size_t)got;
        if(keep>kOutput-1-result->used){keep=kOutput-1-result->used;result->truncated=1;}
        memcpy(result->output+result->used,bytes,keep);result->used+=keep;
    }
    result->output[result->used]=0;
}
static struct result run_case(unsigned index,uint64_t budget_ns)
{
    struct result result={0};result.exit_code=-1;
    if(index>=kCases || !budget_ns || stopping){result.error=stopping?ECANCELED:EINVAL;return result;}
    int ends[2];if(pipe(ends)){result.error=errno;return result;}
    if(fcntl(ends[0],F_SETFL,O_NONBLOCK)){result.error=errno;close(ends[0]);close(ends[1]);return result;}
    uint64_t started=monotonic_ns();pid_t child=started?fork():-1;
    if(child<0){result.error=started?errno:EIO;close(ends[0]);close(ends[1]);return result;}
    if(!child){
        close(ends[0]);
        if(dup2(ends[1],STDOUT_FILENO)<0 || dup2(ends[1],STDERR_FILENO)<0){_exit(120);}
        if(ends[1]>2){close(ends[1]);}
        signal(SIGALRM,SIG_DFL);signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);
        struct rlimit memory={256UL*1024*1024,256UL*1024*1024},cpu={30,31},zero={0,0};
        if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CPU,&cpu) || setrlimit(RLIMIT_CORE,&zero)
            || setrlimit(RLIMIT_FSIZE,&zero)){_exit(121);}
        int status=cases[index]();fflush(NULL);_exit(status>=0 && status<120?status:122);
    }
    close(ends[1]);int status=0;struct rusage usage={0};pid_t waited;
    for(;;){
        drain(ends[0],&result);waited=wait4(child,&status,WNOHANG,&usage);
        if(waited==child){break;}
        if(waited<0 && errno!=EINTR){result.error=errno;break;}
        uint64_t now=monotonic_ns();
        if(result.error || stopping || !now || now<started || now-started>=budget_ns){
            result.timed_out=!stopping && !result.error;
            if(kill(child,SIGKILL) && errno!=ESRCH){result.error=errno;}
            do{waited=wait4(child,&status,0,&usage);}while(waited<0 && errno==EINTR);
            if(waited!=child){result.error=errno?errno:ECHILD;}break;
        }
        struct pollfd ready={ends[0],POLLIN,0};poll(&ready,1,10);
    }
    drain(ends[0],&result);close(ends[0]);uint64_t ended=monotonic_ns();
    result.elapsed_ns=ended>=started?ended-started:0;
    result.user_us=(uint64_t)usage.ru_utime.tv_sec*1000000+(uint64_t)usage.ru_utime.tv_usec;
    result.system_us=(uint64_t)usage.ru_stime.tv_sec*1000000+(uint64_t)usage.ru_stime.tv_usec;
    result.max_rss_kib=usage.ru_maxrss;
    if(waited==child && WIFEXITED(status)){result.exit_code=WEXITSTATUS(status);}
    if(waited==child && WIFSIGNALED(status)){result.signal_number=WTERMSIG(status);}
    result.passed=!stopping && !result.error && !result.timed_out && result.exit_code==0;
    return result;
}
static int escaped(char* target,size_t capacity,const char* source)
{
    size_t used=0;
    for(const unsigned char* p=(const unsigned char*)source;*p;++p){
        int quote=*p=='"' || *p=='\\';
        if(used+(quote?2:1)>=capacity){return 0;}
        if(quote){target[used++]='\\';}
        target[used++]=*p>=32 && *p<127?(char)*p:'?';
    }
    if(used>=capacity){return 0;}target[used]=0;return 1;
}
static int admitted(void)
{
#ifdef PD_CPU_PROBE_HOST
    uid_t expected=getuid();gid_t group=getgid();
#else
    uid_t expected=78666;gid_t group=20;
#endif
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2];
    if(!expected || getuid()!=expected || geteuid()!=expected || getgid()!=group || getegid()!=group
        || syscall(SYS_sched_getscheduler,0)!=SCHED_OTHER || syscall(SYS_capget,&header,caps)){return 0;}
    for(unsigned i=0;i<2;++i){if(caps[i].effective || caps[i].permitted || caps[i].inheritable){return 0;}}
    if(setpriority(PRIO_PROCESS,0,19)){return 0;}
    errno=0;int priority=getpriority(PRIO_PROCESS,0);return !errno && priority==19;
}
static int report(char* output,size_t capacity,const struct result results[kCases],const struct utsname* system)
{
    char kernel[256],machine[256];
    if(!escaped(kernel,sizeof(kernel),system->release) || !escaped(machine,sizeof(machine),system->machine)){return 0;}
    int all=1;for(unsigned i=0;i<kCases;++i){all&=results[i].passed;}
    int n=snprintf(output,capacity,"{\"application\":\"AetherPDCpuProbe\",\"version\":1,\"kernel\":\"%s\",\"machine\":\"%s\",\"uid\":%lu,\"nice\":19,\"passed\":%s,\"rf_commands\":[],\"radio_api_commands\":[],\"tests\":[",kernel,machine,(unsigned long)getuid(),all?"true":"false");
    if(n<0 || (size_t)n>=capacity){return 0;}size_t used=(size_t)n;
    for(unsigned i=0;i<kCases;++i){
        const struct result* r=&results[i];char text[2*kOutput];if(!escaped(text,sizeof(text),r->output)){return 0;}
        n=snprintf(output+used,capacity-used,"%s{\"name\":\"%s\",\"passed\":%s,\"exit\":%d,\"signal\":%d,\"timed_out\":%s,\"errno\":%d,\"elapsed_ns\":%llu,\"user_us\":%llu,\"system_us\":%llu,\"max_rss_kib\":%ld,\"output_truncated\":%s,\"output\":\"%s\"}",i?",":"",names[i],r->passed?"true":"false",r->exit_code,r->signal_number,r->timed_out?"true":"false",r->error,(unsigned long long)r->elapsed_ns,(unsigned long long)r->user_us,(unsigned long long)r->system_us,r->max_rss_kib,r->truncated?"true":"false",text);
        if(n<0 || (size_t)n>=capacity-used){return 0;}used+=(size_t)n;
    }
    return capacity-used>3 && snprintf(output+used,capacity-used,"]}\n")==3;
}
static int close_unrelated(void)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return 0;}int own=dirfd(directory);struct dirent* entry;
    while((entry=readdir(directory))){
        if(entry->d_name[0]=='.'){continue;}char* end;errno=0;long fd=strtol(entry->d_name,&end,10);
        if(errno || *end || fd<0 || fd>INT_MAX){closedir(directory);return 0;}
        if(fd>2 && fd!=own){close((int)fd);}
    }
    return !closedir(directory);
}
static int serve(const char* output)
{
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons(kPort);
    if(listener<0 || bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,2)){if(listener>=0){close(listener);}return 3;}
    uint64_t end=monotonic_ns()+UINT64_C(300000000000);
    while(!stopping && monotonic_ns()<end){
        struct pollfd ready={listener,POLLIN,0};if(poll(&ready,1,200)<=0){continue;}
        int client=accept(listener,NULL,NULL);if(client<0){continue;}
        int flags=fcntl(client,F_GETFL);if(flags<0 || fcntl(client,F_SETFL,flags|O_NONBLOCK)){close(client);continue;}
        size_t sent=0,size=strlen(output);uint64_t deadline=monotonic_ns()+UINT64_C(1000000000);
        while(sent<size && !stopping && monotonic_ns()<deadline){
            ssize_t count=send(client,output+sent,size-sent,MSG_NOSIGNAL);
            if(count>0){sent+=(size_t)count;}else if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK){break;}
            struct pollfd writable={client,POLLOUT,0};if(sent<size){poll(&writable,1,20);}
        }
        close(client);
    }
    close(listener);return 0;
}
int main(int argc,char** argv)
{
    int serving=argc==2 && !strcmp(argv[1],"--serve");
    if((argc!=1 && !serving) || !admitted() || !close_unrelated() || signal(SIGCHLD,SIG_DFL)==SIG_ERR){return 2;}
    signal(SIGTERM,stop_probe);signal(SIGINT,stop_probe);signal(SIGALRM,stop_probe);alarm(600);
    struct utsname system;if(uname(&system)){return 2;}
    struct result results[kCases]={0};
    for(unsigned i=0;i<kCases;++i){results[i].exit_code=-1;}
    for(unsigned i=0;i<kCases && !stopping;++i){results[i]=run_case(i,UINT64_C(45000000000));}
    char output[kReport];if(!report(output,sizeof(output),results,&system)){return 3;}
    if(serving && !stopping){return serve(output);}
    fputs(output,stdout);
    for(unsigned i=0;i<kCases;++i){if(!results[i].passed){return 1;}}
    return 0;
}
