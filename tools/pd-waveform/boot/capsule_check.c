#define _GNU_SOURCE
#include "pd_capsule.h"
#include "pd_worker_units.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char* fault;
int pd_capsule_checkpoint(const char* point)
{
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,point)){raise(SIGKILL);_exit(99);}
    return strncmp(fault,"fail:",5) || strcmp(fault+5,point);
}
uint64_t pd_capsule_test_space(uint64_t actual)
{
    return !strncmp(fault,"space:",6)?strtoull(fault+6,NULL,10):actual;
}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int pd_worker_units_checkpoint(const char* point){return pd_capsule_checkpoint(point);}
static int bytes(const char* path,unsigned char* output,size_t size)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);unsigned char extra;
    int ok=fd>=0 && pread(fd,output,size,0)==(ssize_t)size && pread(fd,&extra,1,(off_t)size)==0;
    if(fd>=0){close(fd);}return ok;
}
int main(int argc,char** argv)
{
    if(argc!=9){return 2;}fault=argv[8];unsigned char key[32],plan[PD_MIGRATION_SIZE];
    if(!bytes(argv[2],key,sizeof(key))){return 2;}
    int parent=open(argv[3],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=open(argv[7],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),ok=0;
    if(!strcmp(argv[1],"publish")){
        int worker=open(argv[4],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        int release=open(argv[5],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        ok=bytes(argv[6],plan,sizeof(plan)) && pd_capsule_publish(parent,worker,release,key,plan);
        if(worker>=0){close(worker);}if(release>=0){close(release);}
    }else if(!strcmp(argv[1],"clean")){
        ok=bytes(argv[6],plan,sizeof(plan)) && pd_capsule_cleanup_retired(parent,store,key,plan);
    }else if(!strcmp(argv[1],"arm") || !strcmp(argv[1],"finish") || !strcmp(argv[1],"reconcile")){
        int units=open(argv[4],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        ok=!strcmp(argv[1],"reconcile")?pd_capsule_reconcile_terminal(parent,units,store,key):
            !strcmp(argv[1],"arm")?pd_worker_units_arm(units):
            bytes(argv[6],plan,sizeof(plan)) && pd_capsule_finish_cleanup(parent,units,store,key,plan);
        if(units>=0){close(units);}
    }else{
        struct pd_capsule capsule;ok=pd_capsule_open(parent,key,&capsule);
        if(ok){
            if(!strcmp(argv[1],"retire")){ok=pd_capsule_retire(&capsule,store);}
            else if(!strcmp(argv[1],"self")){
                int self=open(argv[4],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);ok=pd_capsule_self(&capsule,self);if(self>=0){close(self);}
            }else if(!strcmp(argv[1],"hold")){puts("authenticated");fflush(stdout);char stop;ssize_t n=read(0,&stop,1);(void)n;}
            else if(strcmp(argv[1],"open")){ok=0;}
            pd_capsule_close(&capsule);
        }
    }
    if(parent>=0){close(parent);}if(store>=0){close(store);}return ok?0:1;
}
