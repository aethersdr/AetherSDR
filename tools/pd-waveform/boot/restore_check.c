#define _GNU_SOURCE
#include "pd_predecessor.h"
#include "pd_recovery.h"
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* Test-only Podman resource model; never built into installable helpers. */
static int root=-1,model=-1,trace=-1;
static const char* fault;
static unsigned state[5]; /* original present/running, restored present/running, mount references */
static const char original[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char restored[]="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char layer[]="eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
static const char store_path[]="var/lib/smartsdr/aetherpd";
static int save(const char* operation)
{
    if(pwrite(model,state,sizeof(state),0)!=sizeof(state) || fsync(model)){return 0;}
    char name[80];snprintf(name,sizeof(name),"kill:%s",operation);
    if(!strcmp(fault,name)){raise(SIGKILL);_exit(99);}return 1;
}
static int content(const char* name,unsigned char* bytes,size_t* size)
{
    int fd=openat(root,name,O_RDONLY|O_CLOEXEC);if(fd<0){return 125;}
    ssize_t n=read(fd,bytes,8192);close(fd);if(n<0 || n==8192){return 125;}*size=(size_t)n;return 0;
}
int pd_predecessor_test_command(char* const* args,unsigned char* bytes,size_t* size,int timeout)
{
    (void)timeout;*size=0;
    for(unsigned i=0;args[i];++i){(void)write(trace,args[i],strlen(args[i])+1);}(void)write(trace,"",1);
    if(!strcmp(args[1],"image")){
        if(!strcmp(fault,"image-error")){return 125;}
        return content("image-inventory",bytes,size);
    }
    if(!strcmp(args[1],"container")){
        if(!strcmp(fault,"exists-error")){return 125;}
        if(!strcmp(args[3],original)){return state[0]?0:1;}
        if(!strcmp(args[3],"flex-tailnet-shim")){return state[0] || state[2]?0:1;}return 125;
    }
    if(!strcmp(args[1],"inspect")){
        if(!strncmp(args[3],"PDOVL001",8)){return content(!strcmp(args[4],original)?"overlay-original":"overlay-restored",bytes,size);}
        if(state[0]){return content(state[1]?"original-running":"original-stopped",bytes,size);}
        if(state[2]){return content(state[3]?"restored-running":"restored-stopped",bytes,size);}return 125;
    }
    if(!strcmp(args[1],"create")){
        if(state[0] || state[2] || !strcmp(fault,"create-fail")){return 125;}
        int fd=openat(root,"var/lib/smartsdr/aetherpd/legacy/state/tsnet/tailscaled.state",O_RDONLY|O_CLOEXEC);
        if(fd<0){return 125;}close(fd);state[2]=1;if(!save("create")){return 125;}
        if(!strcmp(fault,"create-timeout")){return -1;}
        memcpy(bytes,restored,64);bytes[64]='\n';*size=65;return 0;
    }
    if(!strcmp(args[1],"rm")){
        if(strcmp(args[2],original) || !state[0] || state[1] || !strcmp(fault,"rm-fail")){return 125;}
        state[0]=0;return save("rm")?0:125;
    }
    if(!strcmp(args[1],"start")){
        if(strcmp(args[2],restored) || !state[2] || state[3] || !strcmp(fault,"start-fail")){return 125;}
        state[3]=1;return save("start")?0:125;
    }
    return 125;
}
int pd_predecessor_test_root(void){return dup(root);}
int pd_predecessor_test_process_root(pid_t pid)
{
    if(pid!=1234){return -1;}char path[256];snprintf(path,sizeof(path),"var/lib/smartsdr/containers/storage/overlay/%s/merged",layer);
    return openat(root,path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
}
int pd_predecessor_test_quiet(void)
{
    struct stat st;return fstatat(root,"run/aetherpd",&st,AT_SYMLINK_NOFOLLOW)<0 &&
        (strcmp(fault,"runtime-after-create") || !state[2]);
}
int pd_store_checkpoint(const char* p){(void)p;return 1;}
uint64_t pd_store_test_space(uint64_t n){return n;}
int pd_transaction_checkpoint(const char* p,unsigned phase){(void)p;(void)phase;return 1;}
int main(int argc,char** argv)
{
    if(argc!=4 || (strcmp(argv[1],"restore") && strcmp(argv[1],"retire") && strcmp(argv[1],"image"))){return 2;}
    fault=argv[3];root=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0){return 2;}model=openat(root,"model",O_RDWR|O_CLOEXEC);trace=openat(root,"trace",O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if(model<0 || trace<0 || pread(model,state,sizeof(state),0)!=sizeof(state)){return 2;}
    int store=openat(root,store_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int input=openat(root,"approval",O_RDONLY|O_CLOEXEC);unsigned char record[PD_MIGRATION_SIZE];struct pd_migration parsed;
    int valid=input>=0 && read(input,record,sizeof(record))==sizeof(record) && pd_migration_parse(record,sizeof(record),&parsed);
    if(input>=0){close(input);}int result=0;
    if(valid){
        if(!strcmp(argv[1],"image")){result=pd_predecessor_image_verify(&parsed.predecessor);}
        else{
            int lock=openat(store,"transaction.lock",O_WRONLY|O_CREAT|O_CLOEXEC,0600);
            if(lock>=0 && !flock(lock,LOCK_EX|LOCK_NB)){
                result=!strcmp(argv[1],"restore")?pd_predecessor_restore(store,record):pd_predecessor_retire(store,record);
            }
            if(lock>=0){close(lock);}
        }
    }
    if(store>=0){close(store);}close(trace);close(model);close(root);return result?0:1;
}
