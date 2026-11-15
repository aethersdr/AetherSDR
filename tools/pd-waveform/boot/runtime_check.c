#define _GNU_SOURCE
#include "pd_runtime.h"
#include "pd_upgrade_state.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>
static int root=-1,after=-1,count=0;
int pd_runtime_test_root(void){return dup(root);}
int pd_runtime_test_quiet(void){return faccessat(root,"busy",F_OK,0)!=0;}
int pd_runtime_checkpoint(void){if(++count==after){raise(SIGKILL);}return 1;}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int main(int argc,char** argv)
{
    if(argc!=3 && argc!=4){return 2;}
    root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=openat(root,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    unsigned char plan[PD_UPGRADE_APPROVAL_SIZE+1];struct pd_migration parsed;struct pd_upgrade upgraded;
    int fd=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    ssize_t size=pread(fd,plan,sizeof(plan),0);
    int upgrade=size==PD_UPGRADE_APPROVAL_SIZE,lock=-1;
    int ok=root>=0 && store>=0 && (upgrade?pd_upgrade_parse(plan,(size_t)size,&upgraded):
        size==PD_MIGRATION_SIZE && pd_migration_parse(plan,(size_t)size,&parsed));
    if(fd>=0){close(fd);}if(argc==4){after=atoi(argv[3]);}
    if(ok && upgrade && faccessat(root,"no-worker",F_OK,0)!=0){
        lock=openat(store,"transaction.lock",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
        ok=lock>=0 && !flock(lock,LOCK_EX|LOCK_NB);
    }
    ok=ok && (upgrade?pd_runtime_upgrade_retire(store,plan):pd_runtime_retire(store,plan));
    if(lock>=0){close(lock);}
    if(store>=0){close(store);}if(root>=0){close(root);}return ok?0:1;
}
