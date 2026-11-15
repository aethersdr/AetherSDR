/* Isolated filesystem/exec adapter; never built into the production entry. */
#define _GNU_SOURCE
#include "pd_delivery.h"
#include "pd_identity.h"
#include "pd_delivery_lease.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

int __wrap_pd_identity_probe(struct pd_identity* result)
{
    static unsigned calls;const char* mode=getenv("PD_DELIVERY_TEST_MODE");++calls;
    memset(result,0,sizeof(*result));strcpy(result->model,"FLEX-8600");strcpy(result->firmware,"4.3.16.42034");
    strcpy(result->serial,mode && (!strcmp(mode,"wrong-target") || (!strcmp(mode,"changed-target") && calls>1))?"wrong":"fixture-8600");
    strcpy(result->address,"192.168.50.100");return 1;
}
static void require(int condition){if(!condition){_exit(91);}}
void pd_delivery_test_exec(const char* path,char* const* args,char* const* env)
{
    require(!strcmp(args[0],"aetherpd-worker") && !strcmp(args[1],"--delivered") && !args[2]);
    require(!(fcntl(7,F_GETFD)&FD_CLOEXEC));
    require(env[0] && !strcmp(env[0],"PATH=/usr/bin:/bin") && !env[1]);
    int executable=atoi(path+strlen("/proc/self/fd/"));struct stat st;
    require(!strncmp(path,"/proc/self/fd/",14) && !fstat(executable,&st) && (st.st_mode&07777)==0700 && st.st_nlink==1);
    char magic[8];require(pread(3,magic,8,0)==8);int upgrade=!memcmp(magic,"PDUPGR01",8);
    require(upgrade || !memcmp(magic,"PDAPRV02",8));
    require(pread(4,magic,8,0)==8 && !memcmp(magic,upgrade?"AEPDR001":"PDMIG001",8));
    int payload=upgrade?5:0;require(pread(payload,magic,8,0)==8 && !memcmp(magic,"AEPDR001",8));
    for(int fd=3;fd<=4;++fd){require((fcntl(fd,F_GETFL)&O_ACCMODE)==O_RDONLY);}
    require((fcntl(payload,F_GETFL)&O_ACCMODE)==O_RDONLY);
    DIR* listing=opendir("/proc/self/fd");require(listing!=NULL);struct dirent* item;
    while((item=readdir(listing))){
        char* end=NULL;long fd=strtol(item->d_name,&end,10);
        if(end==item->d_name || *end){continue;}
        require(fd<=4 || (upgrade && fd==5) || fd==7 || fd==executable || fd==dirfd(listing));
    }
    closedir(listing);
    const char* mode=getenv("PD_DELIVERY_TEST_MODE");
    if(mode && !strcmp(mode,"orphan")){
        char* child_args[]={"delivery-check","--lease-child",NULL};
        execve("/proc/self/exe",child_args,env);_exit(92);
    }
    require(pd_delivery_lease_adopt() && (fcntl(7,F_GETFD)&FD_CLOEXEC));
    if(mode && !strcmp(mode,"foreign-file")){
        char resolved[4096];ssize_t n=readlink(path,resolved,sizeof(resolved)-1);require(n>0);resolved[n]=0;
        char* slash=strrchr(resolved,'/');require(slash!=NULL);strcpy(slash+1,"foreign");
        int file=open(resolved,O_WRONLY|O_CREAT|O_EXCL,0600);require(file>=0);close(file);
    }
    puts("{\"verified_worker_interface\":true}");fflush(stdout);
    if(mode && (!strcmp(mode,"hang") || !strcmp(mode,"orphan"))){for(;;){pause();}}
    _exit(mode && !strcmp(mode,"worker-failure")?7:0);
}
static int pinned(const char* path,int directory)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(directory?O_DIRECTORY:O_NONBLOCK));
    if(fd<0){return -1;}int copy=fcntl(fd,F_DUPFD_CLOEXEC,32);close(fd);return copy;
}
int main(int argc,char** argv)
{
    if(argc==2 && !strcmp(argv[1],"--lease-helper")){
        require(fcntl(7,F_GETFD)<0 && errno==EBADF);return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--lease-child")){
        require(!(fcntl(7,F_GETFD)&FD_CLOEXEC));
        require(pd_delivery_lease_adopt() && (fcntl(7,F_GETFD)&FD_CLOEXEC));
        require(!prctl(PR_SET_PDEATHSIG,0));
        pid_t child=fork();require(child>=0);
        if(child==0){char* args[]={"delivery-check","--lease-helper",NULL};execv("/proc/self/exe",args);_exit(93);}
        int status=0;require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
        puts("{\"verified_worker_interface\":true}");fflush(stdout);
        for(;;){pause();}
    }
    if(argc!=9){return 2;}
    const char* anonymous=getenv("PD_DELIVERY_ANONYMOUS_CONSENT");
    int anonymous_copy=anonymous?fcntl(atoi(anonymous),F_DUPFD_CLOEXEC,128):-1;
    if(anonymous && anonymous_copy<0){return 2;}
    unsigned char key[33];int keyfd=pinned(argv[2],0);
    if(keyfd<0 || read(keyfd,key,sizeof(key))!=32){return 2;}close(keyfd);
    int run=pinned(argv[1],1);if(run<0){return 2;}
    int inputs[5];for(unsigned i=0;i<5;++i){inputs[i]=pinned(argv[3+i],0);if(inputs[i]<0){return 2;}}
    const int target[]={0,3,4,5,6};for(unsigned i=0;i<5;++i){if(dup2(inputs[i],target[i])<0){return 2;}close(inputs[i]);}
    if(anonymous){
        struct stat st;
        if(fstat(anonymous_copy,&st) || st.st_nlink || dup2(anonymous_copy,3)<0){return 2;}
        close(anonymous_copy);
    }
    /* A descriptor outside the fixed ABI must disappear in the child. */
    int extra=open("/dev/null",O_RDONLY);if(extra<0){return 2;}
    struct sigaction action={0};action.sa_handler=pd_delivery_cancel;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,NULL)){return 2;}
    int result=pd_delivery_run(run,key,(uint32_t)strtoul(argv[8],NULL,10));close(extra);close(run);
    printf("{\"delivery_result\":%d}\n",result);return result;
}
