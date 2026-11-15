#define _GNU_SOURCE
#include "pd_migration.h"
#include "pd_readiness.h"
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static const char* source;
static const char* fault;
static int trace=-1,stopped;
int pd_migration_test_readiness(const char* address,struct pd_readiness* result)
{
    (void)address;memset(result,0,sizeof(*result));
    strcpy(result->hostname,!strcmp(fault,"identity-changed")?"other":"fixture");
    strcpy(result->address,"100.64.1.2");strcpy(result->dns,"fixture.example.ts.net");
    return strcmp(fault,"not-ready")!=0;
}
int pd_migration_test_quiet(void)
{
    return strcmp(fault,"runtime") && (strcmp(fault,"runtime-after-stop") || !stopped);
}
static void event(const char* value){(void)write(trace,value,strlen(value));(void)write(trace,"\n",1);}
int pd_store_checkpoint(const char* p){(void)p;return 1;}
uint64_t pd_store_test_space(uint64_t space){return space;}
int pd_migration_checkpoint(const char* point)
{
    if(!strncmp(fault,"kill:",5) && !strcmp(point,fault+5)){(void)kill(getpid(),SIGKILL);_exit(99);}
    return strcmp(point,fault) && !(strcmp(fault,"recovery")==0 && strcmp(point,"stopped")==0);
}
int pd_predecessor_probe(struct pd_predecessor* p)
{
    event("probe");memset(p,0,sizeof(*p));memset(p->container,'a',64);memset(p->image,'b',64);p->pid=42;
    for(unsigned i=0;i<32;++i){p->executable[i]=(unsigned char)i;}
    strcpy(p->radio_address,!strcmp(fault,"address-changed")?"192.168.50.101":"192.168.50.100");
    if(!strcmp(fault,"container")){p->container[0]='c';}
    if(!strcmp(fault,"image")){p->image[0]='c';}
    if(!strcmp(fault,"executable")){p->executable[0]=1;}
    return strcmp(fault,"probe")!=0;
}
int pd_predecessor_image_verify(const struct pd_predecessor* p){(void)p;return strcmp(fault,"image-file")!=0;}
int pd_predecessor_pin(const struct pd_predecessor* p,int* root,int* state,int* ts)
{
    (void)p;event("pin");*root=*state=*ts=-1;if(!strcmp(fault,"pin")){return 0;}
    *root=open(source,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(*root<0){return 0;}*state=dup(*root);*ts=openat(*root,"tsnet",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    return *state>=0 && *ts>=0;
}
int pd_predecessor_stop(const struct pd_predecessor* p,int root)
{
    (void)p;event("stop");if(!strcmp(fault,"stop")){return 0;}stopped=1;
    if(!strcmp(fault,"missing")){return !unlinkat(root,"provision.json",0);}
    if(!strcmp(fault,"rename")){
        int fd=openat(root,"final",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd<0){return 0;}int ok=write(fd,"after-stop",10)==10 && !fsync(fd);close(fd);
        return ok && !renameat(root,"final",root,"provision.json");
    }
    return 1;
}
int pd_predecessor_stopped(const struct pd_predecessor* p)
{
    (void)p;event("stopped");return stopped && strcmp(fault,"unexpected-restart");
}
int pd_predecessor_recover(const struct pd_predecessor* p,int root)
{
    (void)p;(void)root;event("recover");if(!strcmp(fault,"recovery")){return 0;}stopped=0;return 1;
}
int pd_predecessor_recover_fresh(const struct pd_predecessor* p)
{
    (void)p;event("recover-fresh");return strcmp(fault,"recovery")!=0;
}
/* Test adapter; never included in an installable release. */
int main(int argc,char** argv)
{
    if(argc!=6 && !(argc==7 && (!strcmp(argv[6],"recover") || !strcmp(argv[6],"bind")))){return 2;}source=argv[3];fault=argv[5];
    trace=open(argv[4],O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(trace<0){return 2;}
    int record=open(argv[2],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char bytes[PD_MIGRATION_SIZE];struct pd_migration approved;
    int valid=pd_migration_read(record,bytes,&approved);if(record>=0){close(record);}
    if(!valid){close(trace);return 3;}
    int store=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int result=store>=0?(argc==7?(!strcmp(argv[6],"bind")?pd_migration_bind(store,bytes):pd_migration_recover(store,bytes)):pd_migration_copy(store,bytes)):0;
    if(store>=0){close(store);}close(trace);return result==1?0:result<0?7:6;
}
