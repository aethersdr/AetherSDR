#define _GNU_SOURCE
#include "pd_delivery.h"
#include "pd_carrier.h"
#include "pd_transport.h"
#include "pd_endpoint.h"
#include "pd_release_key.h"
#include "monocypher.h"
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

static int receive_request(uid_t sender)
{
    unsigned char key[32],receipt[80];
    if(!pd_transport_config(3,0,key,receipt)){fprintf(stderr,"{\"delivery_error\":\"configuration\"}\n");return 0;}
    close(3);int files[5],copies[5]={-1,-1,-1,-1,-1};
    const char* stage="transport";
    int ok=pd_transport_receive(9,sender,key,receipt,files);close(9);crypto_wipe(key,sizeof(key));
    if(ok){stage="descriptor_copy";}
    for(unsigned i=0;ok && i<5;++i){if(files[i]>=0){copies[i]=fcntl(files[i],F_DUPFD_CLOEXEC,128);ok=copies[i]>=0;}}
    for(unsigned i=0;i<5;++i){if(files[i]>=0){close(files[i]);}}
    if(ok){stage="host_context";ok=pd_carrier_enter(copies[0]);}
    int run=ok?open("/run",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;struct stat st;struct statfs fs;
    if(ok){stage="runtime_directory";}
    ok=ok && run>=0 && !fstat(run,&st) && st.st_uid==0 && !(st.st_mode&0022) &&
        !fstatfs(run,&fs) && (unsigned long)fs.f_type==0x01021994UL;
    int approval=ok?openat(run,".",O_RDWR|O_TMPFILE|O_CLOEXEC,0600):-1;
    if(ok){stage="receipt_write";}
    ok=ok && approval>=0 && !fchmod(approval,0600) &&
        pwrite(approval,receipt,sizeof(receipt),0)==(ssize_t)sizeof(receipt) && !fsync(approval);
    char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",approval);
    int readonly=ok?open(path,O_RDONLY|O_CLOEXEC):-1;
    if(ok){stage="receipt_readonly";}
    int pinned=readonly>=0?fcntl(readonly,F_DUPFD_CLOEXEC,128):-1;
    if(readonly>=0){close(readonly);}if(approval>=0){close(approval);}if(run>=0){close(run);}
    ok=ok && pinned>=0;
    if(ok){
        stage="descriptor_map";
        ok=dup2(pinned,3)>=0 && dup2(copies[1],0)>=0 && dup2(copies[2],4)>=0 && dup2(copies[3],5)>=0;
        if(copies[4]>=0){ok=ok && dup2(copies[4],6)>=0;}else{close(6);}
    }
    for(unsigned i=0;i<5;++i){if(copies[i]>=0){close(copies[i]);}}
    if(pinned>=0){close(pinned);}
    if(!ok){fprintf(stderr,"{\"delivery_error\":\"%s\"}\n",stage);}
    return ok;
}

static int ordinary_schedule(const char** stage)
{
    *stage="scheduler_read";
    /* The ARM Zig/musl scheduling wrappers return ENOSYS; use the same
     * native syscalls as the session launchers. */
    int policy=(int)syscall(SYS_sched_getscheduler,0);struct sched_param current={0};
    if(policy<0 || syscall(SYS_sched_getparam,0,&current)){return 0;}
    if(policy!=SCHED_OTHER || current.sched_priority!=0){
        *stage="scheduler_reset";struct sched_param ordinary={0};
        if(syscall(SYS_sched_setscheduler,0,SCHED_OTHER,&ordinary)){return 0;}
    }
    *stage="nice_read";errno=0;int nice=getpriority(PRIO_PROCESS,0);
    if(errno){return 0;}
    if(nice!=0){*stage="nice_reset";if(setpriority(PRIO_PROCESS,0,0)){return 0;}}
    *stage="working_directory";return !chdir("/");
}

int main(int argc,char** argv)
{
    int receive=argc==2 && !strcmp(argv[1],"--receive");
    int relay=argc==2 && !strcmp(argv[1],"--listen-relay");
    int carrier=argc==2 && !strcmp(argv[1],"--carrier");
    if((argc!=1 && !carrier && !receive && !relay) || getuid()!=0 || geteuid()!=0){return 2;}
#if defined(__aarch64__)
    const uint32_t arch=64;
#elif defined(__arm__)
    if(carrier || receive || relay){return 2;}
    const uint32_t arch=7;
#else
    return 2;
    const uint32_t arch=0;
#endif
    struct sigaction action={0};action.sa_handler=pd_delivery_cancel;sigemptyset(&action.sa_mask);
    pid_t parent=getppid();
    if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL) || sigaction(SIGHUP,&action,NULL) ||
       prctl(PR_SET_PDEATHSIG,SIGTERM) || getppid()!=parent){return 3;}
    umask(077);
    int completion=-1;
    if(relay){
        alarm(125);
        int config=open("/pd/config",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        unsigned char key[32],receipt[80];
        if(config<0 || !pd_transport_config(config,0,key,receipt)){return 3;}
        crypto_wipe(key,sizeof(key));int channel=pd_endpoint_listen(receipt,1);
        int pinned=fcntl(config,F_DUPFD_CLOEXEC,128);close(config);
        if(channel<0 || pinned<0 || dup2(channel,9)<0 || dup2(pinned,3)<0){return 3;}
        if(channel!=9 && channel!=3){close(channel);}close(pinned);
        completion=fcntl(9,F_DUPFD_CLOEXEC,128);if(completion<0){return 3;}
    }
    if((receive || relay) && !receive_request(relay?0:78666)){return 3;}
    if(carrier){int entered=pd_carrier_enter(8);close(8);if(!entered){return 3;}}
    action.sa_handler=SIG_DFL;if(sigaction(SIGCHLD,&action,NULL)){return 3;}
    sigset_t mask;sigemptyset(&mask);if(sigprocmask(SIG_SETMASK,&mask,NULL)){return 3;}
    const char* stage=NULL;
    if(!ordinary_schedule(&stage)){
        fprintf(stderr,"{\"delivery_error\":\"%s\",\"errno\":%d}\n",stage,errno);return 3;
    }
    umask(077);struct stat st;struct statfs fs;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0 || fstat(root,&st) || st.st_uid!=0 || (st.st_mode&0022)){if(root>=0){close(root);}fprintf(stderr,"{\"delivery_error\":\"root_directory\"}\n");return 3;}
    int run=openat(root,"run",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(root);
    if(run<0 || fstatfs(run,&fs) || (unsigned long)fs.f_type!=0x01021994UL){if(run>=0){close(run);}fprintf(stderr,"{\"delivery_error\":\"run_filesystem\"}\n");return 3;}
    const int inputs[]={0,3,4,5};
    for(unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i){
        if(fstatfs(inputs[i],&fs) || (unsigned long)fs.f_type!=0x01021994UL){close(run);fprintf(stderr,"{\"delivery_error\":\"input_filesystem\",\"fd\":%d}\n",inputs[i]);return 3;}
    }
    if(fcntl(6,F_GETFD)>=0 && (fstatfs(6,&fs) || (unsigned long)fs.f_type!=0x01021994UL)){close(run);return 3;}
    int result=pd_delivery_run(run,pd_release_key,arch);close(run);
    printf("{\"worker_succeeded\":%s,\"delivery_result\":%d,\"rf_commands\":[]}\n",result==0?"true":"false",result);
    if(completion>=0){pd_transport_result_send(completion,result);close(completion);}
    return result;
}
