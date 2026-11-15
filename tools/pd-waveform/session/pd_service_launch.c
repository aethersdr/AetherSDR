#define main inherited_shim_program_main
#include "pd_shim_launch.c"
#undef main
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

/* A service manager owns this process group. Only the fixed constructor runs
 * privileged; its log collector drops all authority before reading output. */
static volatile sig_atomic_t service_stopping;
static void service_stop(int number) { (void)number; service_stopping=1; }
static int64_t service_ms(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now)){return -1;}
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
static int service_uuid(const char* text)
{
    if(!text || strlen(text)!=36){return 0;}
    for(unsigned i=0;i<36;++i){
        if(i==8 || i==13 || i==18 || i==23){if(text[i]!='-'){return 0;}}
        else if(!((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f') || (text[i]>='A' && text[i]<='F'))){return 0;}
    }
    return 1;
}
static int service_nonblock(int fd)
{
    int flags=fcntl(fd,F_GETFL);
    return flags>=0 && !fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
static int service_relay(pid_t child,int output,int error)
{
    if(child<=1 || !service_nonblock(output) || !service_nonblock(error)){
        return 8;
    }
    /* Diagnostics may be dropped when the service-manager sink is full. Never
     * let a stalled log consumer back-pressure the waveform's pipe. */
    int sinks[2]={STDOUT_FILENO,STDERR_FILENO};
    for(unsigned i=0;i<2;++i){if(!service_nonblock(sinks[i])){sinks[i]=-1;}}
    struct pollfd streams[2]={{output,POLLIN,0},{error,POLLIN,0}};
    int status=0,exited=0,signaled=0;int64_t stopped=-1,finished=-1;
    for(;;){
        int64_t now=service_ms();if(now<0){return 8;}
        if(service_stopping && !signaled){
            signaled=1;stopped=now;
            if(kill(child,SIGTERM) && errno!=ESRCH){return 8;}
        }
        if(!exited){
            pid_t result=waitpid(child,&status,WNOHANG);
            if(result==child){exited=1;finished=now;}
            else if(result<0 && errno!=EINTR){return 8;}
        }
        if(exited && ((streams[0].fd<0 && streams[1].fd<0) || now-finished>=2000)){
            return WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
        }
        if(signaled && !exited && now-stopped>=10000){
            /* The PID is still our unreaped child. The service manager owns
             * any remaining group cleanup if startup never dropped UID. */
            if(kill(child,SIGKILL) && errno!=ESRCH){return 8;}
            while(waitpid(child,&status,0)<0){if(errno!=EINTR){return 8;}}
            exited=1;finished=now;
        }
        int ready=poll(streams,2,50);
        if(ready<0 && errno!=EINTR){return 8;}
        if(ready<=0){continue;}
        for(unsigned i=0;i<2;++i){
            if(streams[i].fd<0 || !(streams[i].revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))){continue;}
            char buffer[4096];ssize_t count=read(streams[i].fd,buffer,sizeof(buffer));
            if(count>0){
                if(sinks[i]>=0){ssize_t written=write(sinks[i],buffer,(size_t)count);(void)written;}
            }else if(!count || (errno!=EAGAIN && errno!=EINTR)){
                close(streams[i].fd);streams[i].fd=-1;
            }
        }
    }
}
#ifndef PD_SERVICE_MAIN
#define PD_SERVICE_MAIN main
#endif
static int service_keep_descriptors(int output,int error,int installation)
{
    /* The ordinary log collector retains only a read-only installation lock.
     * Its child closes that descriptor before executing the constructor. */
    int kept[6]={0,1,2,output,error,installation<0?0:installation};
    return close_unrelated(kept,-1);
}
static int service_launch(const char* id,uint32_t gui,int control,int installation)
{
    char directory_path[PATH_MAX];int directory=executable_directory(directory_path);
    if(directory<0){return 3;}
    int program=open_program(directory,"pd-radio-launch");close(directory);
    if(program<0){return 3;}
    int output[2],error[2];
    if(pipe2(output,O_CLOEXEC) || pipe2(error,O_CLOEXEC)){return 4;}
    struct sigaction action={.sa_handler=service_stop};sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL) || signal(SIGPIPE,SIG_IGN)==SIG_ERR){return 4;}
    pid_t child=fork();if(child<0){return 4;}
    if(!child){
        int executable=fcntl(program,F_DUPFD_CLOEXEC,64);
        int channel=control<0?-1:fcntl(control,F_DUPFD_CLOEXEC,64);
        int null=open("/dev/null",O_RDONLY|O_CLOEXEC);
        if(executable<0 || null<0 || dup2(null,0)!=0 || dup2(output[1],1)!=1 || dup2(error[1],2)!=2){_exit(5);}
        if(control>=0 && (channel<0 || dup2(channel,3)!=3)){_exit(5);}
        /* Reuse the descriptor allowlist with repeated standard descriptors;
         * no caller-provided descriptor reaches the constructor. */
        int kept[6]={0,1,2,control<0?0:3,1,2};
        if(!close_unrelated(kept,executable)){_exit(5);}
        char identity[96],handle[64];
        snprintf(identity,sizeof(identity),"SHIM_PD_GUI_CLIENT_ID=%s",id);
        snprintf(handle,sizeof(handle),"SHIM_PD_GUI_HANDLE=0x%08x",gui);
        char* env[]={"PATH=/usr/bin:/bin",identity,handle,control<0?NULL:"SHIM_PD_SESSION_FD=3",NULL};
        char* args[]={"pd-radio-launch",NULL};fexecve(executable,args,env);_exit(6);
    }
    close(program);close(output[1]);close(error[1]);
    int null=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if(null<0 || dup2(null,0)!=0 || !service_keep_descriptors(output[0],error[0],installation) || !drop_root()){
        kill(child,SIGTERM);return 7;
    }
    int result=service_relay(child,output[0],error[0]);
    close(output[0]);close(error[0]);return result;
}
int PD_SERVICE_MAIN(int argc,char** argv)
{
    (void)argv;
    uint32_t gui=0;const char* id=getenv("SHIM_PD_GUI_CLIENT_ID");
    if(argc!=1 || getuid()!=0 || geteuid()!=0 || !service_uuid(id) ||
       !launch_handle(getenv("SHIM_PD_GUI_HANDLE"),&gui)){return 2;}
    return service_launch(id,gui,-1,-1);
}
