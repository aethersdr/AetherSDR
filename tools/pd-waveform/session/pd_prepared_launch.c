#define main legacy_shim_launch_main
#include "pd_shim_launch.c"
#undef main
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

// Populated only by the compiled resource constructor from its own forks.
static pid_t prepared_setup_pids[3];
// Set by the fixed constructor, never taken from a prepared-launch request.
static int prepared_session_fd=-1;

static int64_t prepared_ms(void)
{
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){return -1;}return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int prepared_close_except(const int* fds,size_t count,int program)
{
    DIR* d=opendir("/proc/self/fd");if(!d){return 0;}int own=dirfd(d);struct dirent* e;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'){continue;}char* end;errno=0;long fd=strtol(e->d_name,&end,10);
        if(errno || *end || fd<0 || fd>INT_MAX){closedir(d);return 0;}
        int keep=fd<=2 || fd==program || fd==own;
        for(size_t i=0;i<count;++i){keep|=fd==fds[i];}if(!keep){close((int)fd);}
    }
    return !closedir(d);
}
static int prepared_profile(pid_t child,int policy,int priority)
{
    char path[64];snprintf(path,sizeof(path),"/proc/%d/task",child);DIR* d=opendir(path);if(!d){return 0;}
    struct dirent* entry;unsigned threads=0;int valid=1;
    while((entry=readdir(d))){
        if(entry->d_name[0]=='.'){continue;}char* end;errno=0;long tid=strtol(entry->d_name,&end,10);
        if(errno || *end || tid<=0 || tid>INT_MAX){valid=0;break;}
        struct sched_param p={0};int actual=(int)syscall(SYS_sched_getscheduler,(pid_t)tid);
        errno=0;int nice=getpriority(PRIO_PROCESS,(id_t)tid);int failed=errno;
        if(syscall(SYS_sched_getparam,(pid_t)tid,&p) || failed || actual!=(tid==child?policy:SCHED_OTHER) || p.sched_priority!=(tid==child?priority:0) || nice!=(tid==child?0:10)){valid=0;break;}
        ++threads;
    }
    closedir(d);return valid && threads>=3;
}
static int prepared_wait_ready(pid_t child,int gate)
{
    int64_t start=prepared_ms();if(start<0){return 0;}
    for(;;){
        int64_t now=prepared_ms();if(now<start || now-start>=10000){return 0;}
        struct pollfd p={gate,POLLIN,0};int r=poll(&p,1,100);
        if(r<0 && errno==EINTR){continue;}if(r<0 || p.revents&(POLLERR|POLLHUP|POLLNVAL)){return 0;}
        if(p.revents&POLLIN){unsigned char ready[9];ssize_t n=recv(gate,ready,sizeof(ready),MSG_DONTWAIT|MSG_TRUNC);if(n!=8 || memcmp(ready,"PDREADY1",8)){return 0;}break;}
    }
    // Worker entry sets its own ordinary priority. Wait for those already-
    // created threads before promoting only the forked child's main thread.
    start=prepared_ms();if(start<0){return 0;}
    while(!prepared_profile(child,SCHED_OTHER,0)){
        int64_t now=prepared_ms();if(now<start || now-start>=1000){return 0;}
        struct timespec t={0,10000000};nanosleep(&t,NULL);
    }
    return 1;
}
static void prepared_stop_child(pid_t child,int owner)
{
    close(owner);if(child<=0){return;}
    kill(child,SIGTERM);int64_t start=prepared_ms();
    for(;;){
        int status=0;pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child || (result<0 && errno==ECHILD)){return;}
        int64_t now=prepared_ms();if(start<0 || now<start || now-start>=2000){break;}
        struct timespec t={0,10000000};nanosleep(&t,NULL);
    }
    // This PID remains our unreaped child, so it cannot have been reused.
    kill(child,SIGKILL);while(waitpid(child,NULL,0)<0 && errno==EINTR){}
}
static void prepared_exec_dsp(int program,const int source[7],uint32_t gui)
{
    int copies[7];int executable=fcntl(program,F_DUPFD_CLOEXEC,64);if(executable<0){_exit(20);}
    for(unsigned i=0;i<7;++i){copies[i]=fcntl(source[i],F_DUPFD_CLOEXEC,64);if(copies[i]<0){_exit(20);}}
    for(unsigned i=0;i<7;++i){if(dup2(copies[i],(int)i+3)!=(int)i+3){_exit(20);}}
    int null=open("/dev/null",O_RDWR|O_CLOEXEC);if(null<0){_exit(20);}
    // Keep the validated inherited stderr pipe for DSP diagnostics.
    // Input/output remain null; no caller-selected destination is opened.
    for(int fd=0;fd<2;++fd){if(dup2(null,fd)!=fd){_exit(20);}}
    if(null>2){close(null);}
    const int kept[]={3,4,5,6,7,8,9};struct rlimit limit={50000,100000};struct sched_param ordinary={0};
    if(!prepared_close_except(kept,7,executable) || setrlimit(RLIMIT_RTTIME,&limit) ||
       syscall(SYS_sched_setscheduler,0,SCHED_OTHER,&ordinary) || setpriority(PRIO_PROCESS,0,0) || !drop_root()){_exit(21);}
    char handle[64];snprintf(handle,sizeof(handle),"AETHER_PD_GUI_HANDLE=0x%08x",gui);
    char* env[]={"PATH=/usr/bin:/bin","AETHER_PD_RADIO=127.0.0.1","AETHER_PD_FEEDBACK=local","AETHER_PD_TRANSPORT_CACHE=1","AETHER_PD_IQ_FD=3","AETHER_PD_API_FD=4","AETHER_PD_OWNER_FD=6","AETHER_PD_STATUS_FD=7","AETHER_PD_CALIBRATION_FD=8","AETHER_PD_START_FD=9","AETHER_PD_TX_DEADLINE_MS=12",handle,NULL};
    char* args[]={"aether-pd",NULL};fexecve(executable,args,env);_exit(22);
}
#ifndef PD_PREPARED_MAIN
#define PD_PREPARED_MAIN main
#endif
int PD_PREPARED_MAIN(int argc,char** argv)
{
    if(argc!=7 || geteuid()!=0 || !standard_streams() || syscall(SYS_sched_getscheduler,0)!=SCHED_OTHER){return 2;}
    int fds[6];for(unsigned i=0;i<6;++i){fds[i]=launch_fd(argv[i+1]);if(fds[i]<0){return 2;}}
    uint32_t client,gui;
    if(!launch_handle(getenv("SHIM_PD_UNKEY_CLIENT_HANDLE"),&client) || !launch_handle(getenv("SHIM_PD_GUI_HANDLE"),&gui) || client==gui || !handoff_roles(fds)){return 3;}
    char directory_path[PATH_MAX];int directory=executable_directory(directory_path);if(directory<0){return 4;}
    int program=open_program(directory,kShim),dsp=open_program(directory,"aether-pd");
    const char* helpers[]={"pd-unkey-guard","pd-lease-guard"};
    for(unsigned i=0;i<2;++i){int fd=open_program(directory,helpers[i]);if(fd<0){close(directory);if(program>=0){close(program);}if(dsp>=0){close(dsp);}return 4;}close(fd);}
    if(program<0 || dsp<0 || !check_data(directory,"state") || !check_data(directory,"run")){close(directory);if(program>=0){close(program);}if(dsp>=0){close(dsp);}return 4;}
    close(directory);
    char state[PATH_MAX],run[PATH_MAX];int s=snprintf(state,sizeof(state),"%s/state",directory_path),r=snprintf(run,sizeof(run),"%s/run",directory_path);
    if(s<=0 || s>=(int)sizeof(state) || r<=0 || r>=(int)sizeof(run)){close(program);close(dsp);return 4;}
    int owner[2],status[2],calibration[2],gate[2];
    if(pipe2(owner,O_CLOEXEC) || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,status) ||
       socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,calibration) || socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0,gate)){return 5;}
    const int allocated[]={owner[0],owner[1],status[0],status[1],calibration[0],calibration[1],gate[0],gate[1]};
    for(unsigned i=0;i<8;++i){if(allocated[i]<3 || allocated[i]>1023){return 5;}}
    const int source[]={fds[0],fds[1],fds[2],owner[0],status[1],calibration[1],gate[1]};
    pid_t child=fork();if(child<0){return 5;}if(!child){prepared_exec_dsp(dsp,source,gui);}
    close(dsp);close(owner[0]);close(status[1]);close(calibration[1]);close(gate[1]);
    struct sched_param realtime={.sched_priority=1};
    if(!prepared_wait_ready(child,gate[0]) || syscall(SYS_sched_setscheduler,child,SCHED_RR,&realtime) || !prepared_profile(child,SCHED_RR,1)){prepared_stop_child(child,owner[1]);return 7;}
    const char* keys[]={"SHIM_PD_IQ_FD","SHIM_PD_API_FD","SHIM_PD_GUARD_FD","SHIM_PD_SETUP_FD","SHIM_PD_UNKEY_API_FD","SHIM_PD_GUARD_READ_FD"};
    char entries[17][96];char* env[21]={"PATH=/usr/bin:/bin","SHIM_PD_OWN_LEASE_GUARD=1","SHIM_PD_TRANSPORT_CACHE=1"};
    for(unsigned i=0;i<6;++i){snprintf(entries[i],sizeof(entries[i]),"%s=%d",keys[i],fds[i]);env[3+i]=entries[i];}
    snprintf(entries[6],sizeof(entries[6]),"SHIM_PD_UNKEY_CLIENT_HANDLE=0x%08x",client);env[9]=entries[6];
    snprintf(entries[7],sizeof(entries[7]),"SHIM_PD_GUI_HANDLE=0x%08x",gui);env[10]=entries[7];
    snprintf(entries[8],sizeof(entries[8]),"SHIM_PD_DSP_PID=%d",child);env[11]=entries[8];
    const int prepared[]={owner[1],status[0],calibration[0],gate[0]};const char* prepared_keys[]={"SHIM_PD_OWNER_FD","SHIM_PD_STATUS_FD","SHIM_PD_CALIBRATION_FD","SHIM_PD_START_FD"};
    int kept[11];unsigned kept_count=10;memcpy(kept,fds,sizeof(fds));memcpy(kept+6,prepared,sizeof(prepared));
    for(unsigned i=0;i<4;++i){snprintf(entries[9+i],sizeof(entries[9+i]),"%s=%d",prepared_keys[i],prepared[i]);env[12+i]=entries[9+i];}
    if(prepared_setup_pids[0]>1 && prepared_setup_pids[1]>1 && prepared_setup_pids[2]>1){
        const char* setup_keys[]={"SHIM_PD_SOURCE_PID","SHIM_PD_CAPTURE_PID","SHIM_PD_ARP_PID"};
        for(unsigned i=0;i<3;++i){snprintf(entries[13+i],sizeof(entries[13+i]),"%s=%ld",setup_keys[i],(long)prepared_setup_pids[i]);env[16+i]=entries[13+i];}
    }
    if(prepared_session_fd>=3){
        // This role exists only in the full constructor with owned setup children.
        if(!env[16] || !env[17] || !env[18]){prepared_stop_child(child,owner[1]);return 5;}
        kept[kept_count++]=prepared_session_fd;
        snprintf(entries[16],sizeof(entries[16]),"SHIM_PD_CONTROL_FD=%d",prepared_session_fd);env[19]=entries[16];
    }
    if(!prepared_close_except(kept,kept_count,program) || !drop_root()){prepared_stop_child(child,owner[1]);return 5;}
    for(unsigned i=0;i<kept_count;++i){int flags=fcntl(kept[i],F_GETFD);if(flags<0 || fcntl(kept[i],F_SETFD,flags&~FD_CLOEXEC)){prepared_stop_child(child,owner[1]);return 5;}}
    char* args[]={"flex-tailnet-shim","-state",state,"-run-dir",run,"-radio","127.0.0.1","-api","192.168.50.100:48997",NULL};
    if(prepared_session_fd>=3){args[1]=NULL;}
    fexecve(program,args,env);prepared_stop_child(child,owner[1]);return 6;
}
