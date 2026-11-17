#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>

/* Only self/child restrictions, clocks and ordinary sockets. No radio API,
 * raw socket, network configuration, filesystem mutation or RF commands. */
struct probe_result { int result,error,child_ok; };
enum { kReportCapacity=4096,kFilesystemCapacity=320 };
static volatile sig_atomic_t stopping;
static void stop_probe(int number){(void)number;stopping=1;}
#include "pd_pi_probe.h"

static struct probe_result child_restriction(int operation)
{
    struct probe_result result={-1,0,0};int ends[2];
    if(pipe(ends)){result.error=errno;return result;}
    pid_t child=fork();
    if(child<0){result.error=errno;close(ends[0]);close(ends[1]);return result;}
    if(!child){
        close(ends[0]);struct probe_result reply={0,0,0};
        if(prctl(PR_SET_DUMPABLE,0,0,0,0)){reply.result=-1;reply.error=errno;}
        else{
            reply.result=operation==1?prctl(PR_SET_SECCOMP,1,0,0,0):prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
            if(reply.result){reply.error=errno;}
        }
        /* Strict seccomp permits write and SYS_exit, not exit_group. */
        ssize_t sent=write(ends[1],&reply,sizeof(reply));
        syscall(SYS_exit,sent==(ssize_t)sizeof(reply)?0:1);
        __builtin_unreachable();
    }
    close(ends[1]);struct pollfd ready={ends[0],POLLIN,0};int polled;
    do{polled=poll(&ready,1,2000);}while(polled<0 && errno==EINTR && !stopping);
    int got=0;
    if(polled>0 && (ready.revents&POLLIN)){
        got=read(ends[0],&result,sizeof(result))==(ssize_t)sizeof(result);
    }
    if(!got){result=(struct probe_result){-1,polled==0?ETIMEDOUT:EIO,0};kill(child,SIGKILL);}
    close(ends[0]);int status=0;pid_t waited;
    do{waited=waitpid(child,&status,0);}while(waited<0 && errno==EINTR);
    result.child_ok=got && waited==child && WIFEXITED(status) && WEXITSTATUS(status)==0;
    return result;
}

static int timer_probe(int* error)
{
    int fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(fd<0){*error=errno;return 0;}
    struct itimerspec period={{0,0},{0,1000000}};int ok=0;
    if(timerfd_settime(fd,0,&period,NULL)){*error=errno;}
    else{
        struct pollfd ready={fd,POLLIN,0};int polled;
        do{polled=poll(&ready,1,250);}while(polled<0 && errno==EINTR && !stopping);
        uint64_t ticks=0;
        ok=polled>0 && (ready.revents&POLLIN) && read(fd,&ticks,sizeof(ticks))==(ssize_t)sizeof(ticks) && ticks>0;
        if(!ok){*error=polled==0?ETIMEDOUT:EIO;}
    }
    close(fd);return ok;
}

static int escaped(char* out,size_t capacity,const char* text)
{
    size_t used=0;
    for(const unsigned char* p=(const unsigned char*)text;*p;++p){
        if(*p<' ' || *p>=127){if(used+1>=capacity){return 0;}out[used++]='?';}
        else{
            if(*p=='"' || *p=='\\'){if(used+1>=capacity){return 0;}out[used++]='\\';}
            if(used+1>=capacity){return 0;}out[used++]=(char)*p;
        }
    }
    out[used]=0;return 1;
}

static int limit_json(char* out,size_t capacity,const struct rlimit* value,int error)
{
    int n=snprintf(out,capacity,"{\"soft\":%" PRIu64 ",\"hard\":%" PRIu64 ",\"soft_unlimited\":%s,\"hard_unlimited\":%s,\"errno\":%d}",
        value->rlim_cur==RLIM_INFINITY?UINT64_C(0):(uint64_t)value->rlim_cur,
        value->rlim_max==RLIM_INFINITY?UINT64_C(0):(uint64_t)value->rlim_max,
        value->rlim_cur==RLIM_INFINITY?"true":"false",value->rlim_max==RLIM_INFINITY?"true":"false",error);
    return n>0 && (size_t)n<capacity?n:0;
}

static int filesystem_json(char* out,size_t capacity,const char* path,
    const struct statfs* kind,int kind_error,const struct statvfs* space,int space_error)
{
    char name[128],type[32],bytes[32],inodes[32],block[32];
    if(!escaped(name,sizeof(name),path)){return 0;}
    if(kind_error){strcpy(type,"null");}
    else{snprintf(type,sizeof(type),"\"0x%08lx\"",(unsigned long)kind->f_type);}
    if(!space_error && (!space->f_frsize || (uint64_t)space->f_bavail>UINT64_MAX/(uint64_t)space->f_frsize)){space_error=EOVERFLOW;}
    if(space_error){strcpy(bytes,"null");strcpy(inodes,"null");strcpy(block,"null");}
    else{
        snprintf(bytes,sizeof(bytes),"%" PRIu64,(uint64_t)space->f_bavail*(uint64_t)space->f_frsize);
        snprintf(inodes,sizeof(inodes),"%" PRIu64,(uint64_t)space->f_favail);
        snprintf(block,sizeof(block),"%" PRIu64,(uint64_t)space->f_frsize);
    }
    const char* tmpfs=kind_error?"null":(unsigned long)kind->f_type==0x01021994UL?"true":"false";
    int n=snprintf(out,capacity,"{\"path\":\"%s\",\"type\":%s,\"type_errno\":%d,\"tmpfs\":%s,\"space_errno\":%d,\"available_bytes\":%s,\"available_inodes\":%s,\"block_bytes\":%s}",
        name,type,kind_error,tmpfs,space_error,bytes,inodes,block);
    return n>0 && (size_t)n<capacity?n:0;
}
static int filesystem_probe(char* out,size_t capacity,const char* path)
{
    struct statfs kind={0};struct statvfs space={0};
    int kind_error=statfs(path,&kind)?errno:0;
    int space_error=statvfs(path,&space)?errno:0;
    return filesystem_json(out,capacity,path,&kind,kind_error,&space,space_error);
}

static int report(char* out,size_t capacity)
{
    struct utsname system;char release[160],machine[160];
    if(uname(&system) || !escaped(release,sizeof(release),system.release) || !escaped(machine,sizeof(machine),system.machine)){return 0;}
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct caps[2]={{0}};
    int cap_error=syscall(SYS_capget,&header,caps)?errno:0;
    uint64_t effective=(uint64_t)caps[1].effective<<32|caps[0].effective;
    uint64_t permitted=(uint64_t)caps[1].permitted<<32|caps[0].permitted;
    uint64_t inheritable=(uint64_t)caps[1].inheritable<<32|caps[0].inheritable;
    errno=0;int inherited_seccomp=prctl(PR_GET_SECCOMP,0,0,0,0),seccomp_error=inherited_seccomp<0?errno:0;
    struct probe_result strict=child_restriction(1),nnp=child_restriction(2);
    struct pi_probe_result pi=pi_mutex_probe();
    int timer_error=0,timer_ok=timer_probe(&timer_error);struct timespec stamp;
    errno=0;int clock_ok=clock_gettime(CLOCK_MONOTONIC,&stamp)==0,clock_error=clock_ok?0:errno;
    struct rlimit rtprio={0},rttime={0},nice_limit={0};
    errno=0;int rtprio_error=getrlimit(RLIMIT_RTPRIO,&rtprio)?errno:0;
    errno=0;int rttime_error=getrlimit(RLIMIT_RTTIME,&rttime)?errno:0;
    errno=0;int nice_limit_error=getrlimit(RLIMIT_NICE,&nice_limit)?errno:0;
    char rtprio_json[192],rttime_json[192],nice_json[192];
    if(!limit_json(rtprio_json,sizeof(rtprio_json),&rtprio,rtprio_error) ||
       !limit_json(rttime_json,sizeof(rttime_json),&rttime,rttime_error) ||
       !limit_json(nice_json,sizeof(nice_json),&nice_limit,nice_limit_error)){return 0;}
    // The linked musl sched_getscheduler/getparam wrappers are ENOSYS stubs.
    // Query the Linux ABI so a library stub is not reported as kernel absence.
    errno=0;int policy=(int)syscall(SYS_sched_getscheduler,0),policy_error=policy<0?errno:0;
    struct sched_param priority={0};errno=0;int priority_error=syscall(SYS_sched_getparam,0,&priority)?errno:0;
    errno=0;int nice_value=getpriority(PRIO_PROCESS,0),nice_error=errno;
    const char* paths[]={"/run","/run/aetherpd","/tmp","/dev/shm","/user","/var/tmp"};
    char filesystems[6][kFilesystemCapacity];
    for(unsigned i=0;i<6;++i){if(!filesystem_probe(filesystems[i],sizeof(filesystems[i]),paths[i])){return 0;}}
    int n=snprintf(out,capacity,
        "{\"application\":\"AetherPDKernelProbe\",\"version\":1,\"kernel\":\"%s\",\"machine\":\"%s\",\"uid\":%lu,\"euid\":%lu,"
        "\"gid\":%lu,\"egid\":%lu,\"capget_errno\":%d,\"cap_effective\":\"%016" PRIx64 "\","
        "\"cap_permitted\":\"%016" PRIx64 "\",\"cap_inheritable\":\"%016" PRIx64 "\","
        "\"managed_uid_caps_eligible\":%s,\"inherited_seccomp_mode\":%d,\"inherited_seccomp_errno\":%d,"
        "\"strict_seccomp\":{\"entered\":%s,\"errno\":%d,\"child_exited_cleanly\":%s},"
        "\"no_new_privs\":{\"set\":%s,\"errno\":%d,\"child_exited_cleanly\":%s},"
        "\"contended_recursive_pi_mutex\":{\"ok\":%s,\"errno\":%d,\"child_exited_cleanly\":%s},"
        "\"monotonic_clock\":{\"ok\":%s,\"errno\":%d},\"timerfd_tick\":{\"ok\":%s,\"errno\":%d},"
        "\"scheduling\":{\"policy\":%d,\"policy_errno\":%d,\"priority\":%d,\"priority_errno\":%d,\"nice\":%d,\"nice_errno\":%d},"
        "\"rtprio_limit\":%s,\"rttime_limit_us\":%s,\"nice_limit\":%s,"
        "\"filesystems\":[%s,%s,%s,%s,%s,%s],"
        "\"packet_ring_runtime_tested\":false,\"setup_privileges_provisioned\":false,\"rf_commands\":[]}\n",
        release,machine,(unsigned long)getuid(),(unsigned long)geteuid(),(unsigned long)getgid(),(unsigned long)getegid(),
        cap_error,effective,permitted,inheritable,
        !cap_error && getuid()!=0 && geteuid()!=0 && !effective && !permitted && !inheritable?"true":"false",
        inherited_seccomp,seccomp_error,strict.result==0 && strict.child_ok?"true":"false",strict.error,strict.child_ok?"true":"false",
        nnp.result==0 && nnp.child_ok?"true":"false",nnp.error,nnp.child_ok?"true":"false",
        pi.ok?"true":"false",pi.error,pi.child_ok?"true":"false",
        clock_ok?"true":"false",clock_error,timer_ok?"true":"false",timer_error,
        policy,policy_error,priority.sched_priority,priority_error,nice_value,nice_error,rtprio_json,rttime_json,nice_json,
        filesystems[0],filesystems[1],filesystems[2],filesystems[3],filesystems[4],filesystems[5]);
    return n>0 && (size_t)n<capacity?n:0;
}

static int serve(const char* output,size_t length)
{
    int listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0){return 2;}
    int enabled=1;struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(48998),.sin_addr={htonl(INADDR_ANY)}};
    if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled)) ||
       fcntl(listener,F_SETFL,O_NONBLOCK) || bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,4)){close(listener);return 2;}
    struct sigaction action={.sa_handler=stop_probe};sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL)){close(listener);return 2;}
    while(!stopping){
        struct pollfd ready={listener,POLLIN,0};int polled=poll(&ready,1,500);
        if(polled<0 && errno==EINTR){continue;}if(polled<0){close(listener);return 2;}if(!polled){continue;}
        int client=accept(listener,NULL,NULL);if(client<0){continue;}
        /* No request is read or interpreted; one immutable report then EOF. */
        size_t sent=0;
        while(sent<length){
            ssize_t n=send(client,output+sent,length-sent,MSG_DONTWAIT|MSG_NOSIGNAL);
            if(n>0){sent+=(size_t)n;continue;}if(n<0 && errno==EINTR && !stopping){continue;}break;
        }
        close(client);struct timespec limit={0,100000000};nanosleep(&limit,NULL);
    }
    close(listener);return 0;
}

int main(int argc,char** argv)
{
    if(argc>2 || (argc==2 && strcmp(argv[1],"--serve"))){return 2;}
    char output[kReportCapacity];int length=report(output,sizeof(output));if(!length){return 2;}
    if(fwrite(output,1,(size_t)length,stdout)!=(size_t)length || fflush(stdout)){return 2;}
    return argc==2?serve(output,(size_t)length):0;
}
