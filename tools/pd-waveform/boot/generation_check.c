/* Host-only descriptor adapter; all paths and faults belong to disposable tests. */
#define _GNU_SOURCE
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_capsule_io.h"
#include "pd_store.h"
#include "pd_retained.h"
#include "pd_retire.h"
#include "pd_slots_retire.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
static const char* fault;
int pd_store_checkpoint(const char* point){(void)point;return 1;}
uint64_t pd_store_test_space(uint64_t value){return value;}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
uint64_t pd_upgrade_test_free(uint64_t value,int inodes){(void)inodes;return value;}
int pd_upgrade_checkpoint(const char* point,unsigned index)
{
    char name[96];snprintf(name,sizeof(name),"%s:%u",point,index);
    if(!strcmp(fault,name)){raise(SIGKILL);}return 1;
}
int pd_capsule_checkpoint(const char* point)
{
    if(!strcmp(fault,point)){raise(SIGKILL);}return 1;
}
int main(int argc,char** argv)
{
    if(argc!=9){return 2;}fault=argv[8];
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=openat(root,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int units=openat(root,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int input[4];for(unsigned i=0;i<4;++i){input[i]=open(argv[i+2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);}
    unsigned char key[33],bytes[PD_UPGRADE_APPROVAL_SIZE];struct pd_identity identity;
    struct pd_upgrade approved;struct pd_upgrade_origin origin;struct pd_generation generation={.directory=-1,.stage=-1,.control=-1};
    int ok=root>=0 && store>=0 && input[3]>=0 && read(input[3],key,sizeof(key))==32 &&
        pd_identity_parse("R970201|0|model=FLEX-8600,software_ver=4.3.16.42034,chassis_serial=fixture-8600",&identity);
    int lock=pd_store_lock(store),transaction=-1;ok=ok && lock>=0;
    if(ok && !strcmp(argv[6],"stage")){
        ok=pd_upgrade_inspect_origin(store,units,input[2],input[0],input[1],key,64,&identity,&approved,&origin) &&
            pd_generation_open(store,&approved,1,&generation) &&
            pd_upgrade_resume(generation.stage,input[0],input[1],key,64,&identity,&approved) &&
            pd_pair_publish(generation.control,generation.stage,key,64,&identity,PD_PAIR_PREVIOUS);
        if(ok){
            printf("{\"origin\":\"");for(unsigned i=0;i<sizeof(origin.record);++i){printf("%02x",origin.record[i]);}puts("\"}");
        }
    }else if(ok && !strcmp(argv[6],"select")){
        int fd=open(argv[7],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);unsigned char raw[PD_UPGRADE_ORIGIN_SIZE];
        ok=pd_capsule_io_read(input[2],bytes,sizeof(bytes),0) && pd_upgrade_parse(bytes,sizeof(bytes),&approved) &&
            pd_capsule_io_read(fd,raw,sizeof(raw),0) && pd_upgrade_origin_parse(raw,sizeof(raw),&approved,&origin);
        if(fd>=0){close(fd);}transaction=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(strcmp(fault,"no-lock")){ok=ok && transaction>=0 && !flock(transaction,LOCK_EX|LOCK_NB);}
        ok=ok && pd_generation_select(store,&approved,&origin,64,&identity);
    }else if(ok && !strcmp(argv[6],"retire")){
        int fd=open(argv[7],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);unsigned char raw[PD_UPGRADE_ORIGIN_SIZE];
        ok=pd_capsule_io_read(input[2],bytes,sizeof(bytes),0) && pd_upgrade_parse(bytes,sizeof(bytes),&approved) &&
            pd_capsule_io_read(fd,raw,sizeof(raw),0) && pd_upgrade_origin_parse(raw,sizeof(raw),&approved,&origin);
        if(fd>=0){close(fd);}transaction=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(strcmp(fault,"no-lock")){ok=ok && transaction>=0 && !flock(transaction,LOCK_EX|LOCK_NB);}
        ok=ok && pd_retire_predecessor(store,key,64,&identity,&origin);
    }else if(ok && !strcmp(argv[6],"slots-retire")){
        transaction=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(strcmp(fault,"no-lock")){ok=ok && transaction>=0 && !flock(transaction,LOCK_EX|LOCK_NB);}
        ok=ok && pd_slots_retire(store,key,64,&identity);
    }else if(ok && !strcmp(argv[6],"compact")){
        transaction=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(strcmp(fault,"no-lock")){ok=ok && transaction>=0 && !flock(transaction,LOCK_EX|LOCK_NB);}
        ok=ok && pd_retained_compact(store,key,64,&identity);
    }else if(ok && !strcmp(argv[6],"retire-ready")){
        struct pd_upgrade_state state;
        ok=pd_upgrade_state_read(store,&state)==1 && pd_retire_ready(store,&state);
    }else if(ok && !strcmp(argv[6],"retained-read")){
        struct pd_upgrade_state state;
        ok=pd_upgrade_state_read(store,&state)==1 && pd_generation_active(store,&generation)==1 &&
            pd_retained_reopen(store,generation.stage,key,64,&identity,&state,&approved);
    }else if(ok && !strcmp(argv[6],"candidate")){
        ok=pd_capsule_io_read(input[2],bytes,sizeof(bytes),0) && pd_upgrade_parse(bytes,sizeof(bytes),&approved) &&
            pd_generation_open(store,&approved,0,&generation) &&
            pd_pair_publish(generation.control,generation.stage,key,64,&identity,PD_PAIR_CANDIDATE);
    }else if(ok && !strcmp(argv[6],"active")){
        int selected=pd_generation_active(store,&generation);struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};
        ok=selected>=0 && (!selected || pd_pair_acquire(generation.control,generation.stage,key,64,&identity,&pair));
        if(ok){printf("{\"selected\":%d,\"legacy\":%d,\"choice\":%u}\n",selected,generation.legacy,pair.choice);}
        pd_pair_close(&pair);
    }else{ok=0;}
    pd_generation_close(&generation);if(transaction>=0){close(transaction);}if(lock>=0){close(lock);}
    for(unsigned i=0;i<4;++i){if(input[i]>=0){close(input[i]);}}
    if(units>=0){close(units);}if(store>=0){close(store);}if(root>=0){close(root);}return ok?0:1;
}
