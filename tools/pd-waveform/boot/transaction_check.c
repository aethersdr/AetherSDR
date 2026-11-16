#define _GNU_SOURCE
#include "pd_transaction.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Test-only resource model. Production adapters must inspect actual resources. */
struct resources {
    unsigned old_present,old_running,new_running,old_identity,new_identity,authority;
    unsigned prepared,copied,retired,activated,rollbacks,cleanups;
};
static int model=-1;
static int store_descriptor=-1;
static const char* fault;
static struct resources state;
static int save(void){return pwrite(model,&state,sizeof(state),0)==sizeof(state) && !fsync(model);}
static int finish(const char* action)
{
    if(!save()){return 0;}char point[80];snprintf(point,sizeof(point),"kill:%s",action);
    if(!strcmp(fault,point)){raise(SIGKILL);_exit(99);}
    snprintf(point,sizeof(point),"fail:%s",action);return strcmp(fault,point)!=0;
}
int pd_transaction_checkpoint(const char* point,unsigned phase)
{
    char name[80];snprintf(name,sizeof(name),"kill:%s:%u",point,phase);
    if(!strcmp(fault,name)){raise(SIGKILL);_exit(99);}
    snprintf(name,sizeof(name),"close:%s:%u",point,phase);
    if(!strcmp(fault,name)){close(store_descriptor);store_descriptor=-1;}
    snprintf(name,sizeof(name),"fail:%s:%u",point,phase);return strcmp(fault,name)!=0;
}
static int prepare(void* p)
{
    (void)p;if(!state.authority || !state.old_present || !state.old_running || state.new_running){return 0;}
    ++state.prepared;return finish("prepare");
}
static int copy(void* p)
{
    (void)p;if(!state.prepared || !state.old_running || state.new_running){return 0;}
    state.old_running=0;state.new_identity=state.old_identity;++state.copied;return finish("copy");
}
static int retire(void* p)
{
    (void)p;if(!state.copied || state.old_running || state.new_running){return 0;}
    state.old_present=0;++state.retired;return finish("retire");
}
static int activate(void* p)
{
    (void)p;if(state.old_present || state.old_running || !state.retired){return 0;}
    state.new_running=1;++state.new_identity;++state.activated;return finish("activate");
}
static int rollback(void* p)
{
    (void)p;
    if(state.new_running){state.old_identity=state.new_identity;}
    state.new_running=0;state.old_present=state.old_running=1;++state.rollbacks;return finish("rollback");
}
static int cleanup(void* p)
{
    (void)p;if(state.old_running==state.new_running){return 0;}
    state.authority=0;++state.cleanups;return finish("cleanup");
}
int main(int argc,char** argv)
{
    if(argc!=5 && !(argc==6 && (!strcmp(argv[5],"recover") || !strcmp(argv[5],"authorize-retry")))){return 2;}fault=argv[4];unsigned char record[PD_MIGRATION_SIZE];
    int source=open(argv[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(source<0 || pread(source,record,sizeof(record),0)!=sizeof(record)){if(source>=0){close(source);}return 2;}close(source);
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    store_descriptor=store;
    model=open(argv[3],O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(store<0 || model<0 || pread(model,&state,sizeof(state),0)!=sizeof(state)){return 2;}
    const struct pd_transaction_ops ops={NULL,prepare,copy,retire,activate,rollback,cleanup};
    const struct pd_transaction_ops recover_ops={NULL,NULL,NULL,NULL,NULL,rollback,cleanup};
    int result=argc==6 && !strcmp(argv[5],"authorize-retry")?(pd_transaction_authorize_retry(store,record)?1:-2):
        argc==6?pd_transaction_recover(store,record,&recover_ops):pd_transaction_run(store,record,&ops);
    close(model);if(store_descriptor>=0){close(store_descriptor);}
    return result==1?0:result==0?10:result==-1?11:12;
}
