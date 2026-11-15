#define _GNU_SOURCE
#include "pd_service.h"
#include "pd_transaction.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int root=-1,store=-1,lease=-1,started,waited,open_failed,upgrade;
static const char* fault;
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int pd_service_test_root(void){return dup(root);}
static void event(const char* text)
{
    int fd=openat(root,"events",O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC,0600);
    assert(fd>=0 && dprintf(fd,"%s\n",text)>0 && !close(fd));
}
static int input(const char* name,unsigned char* bytes,size_t* size)
{
    int fd=openat(root,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){return 0;}ssize_t n=read(fd,bytes,PD_SERVICE_OUTPUT_MAX);close(fd);
    if(n<0){return 0;}*size=(size_t)n;return 1;
}
int pd_service_test_command(char* const* args,unsigned char* bytes,size_t* size,int timeout)
{
    (void)timeout;int log=openat(root,"commands",O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC,0600);
    assert(log>=0);
    for(unsigned i=0;args[i];++i){assert(dprintf(log,"%s%s",i?" ":"",args[i])>=0);}
    assert(dprintf(log,"\n")>0 && !close(log));*size=0;
    if(!strcmp(args[1],"show")){
        char name[192];const char* prefix=started?"after-":"before-";
        if(started && !strcmp(args[3],"aetherpd-shim.service")){
            if(open_failed){prefix="before-";}
            else if(waited && !strcmp(fault,"changed-pid")){prefix="changed-";}
        }
        snprintf(name,sizeof(name),"%s%s",prefix,args[3]);return input(name,bytes,size);
    }
    if(strcmp(args[1],"start") || args[3] ||
        (strcmp(args[2],"aetherpd-boot.service") && strcmp(args[2],"aetherpd-shim.service"))){return 0;}
    if(!strcmp(fault,"command-fails")){return 0;}
    started=1;event("start");
    if(!strcmp(fault,"worker-dies")){assert(lease>=0 && !flock(lease,LOCK_UN));}
    if(!strcmp(fault,"phase-changes")){
        int fd=openat(store,upgrade?"upgrade-transaction":"transaction",O_WRONLY|O_CLOEXEC);
        assert(fd>=0);off_t end=lseek(fd,-1,SEEK_END);unsigned char phase=upgrade?5:6;
        assert(end>=0 && write(fd,&phase,1)==1 && !close(fd));
    }
    if(!strcmp(fault,"unit-changes")){
        int fd=openat(root,"etc/systemd/system/aetherpd-shim.service",O_WRONLY|O_TRUNC|O_CLOEXEC);
        assert(fd>=0 && write(fd,"foreign",7)==7 && !close(fd));
    }
    return 1;
}
int __wrap_pd_primary_open(pid_t pid,int executable,struct pd_primary* primary)
{
    event("open");assert(pid==42 && executable>=0);
    if(!strcmp(fault,"reject-process")){open_failed=1;return 0;}
    primary->pid=pid;primary->process=dup(root);primary->executable=dup(executable);
    return primary->process>=0 && primary->executable>=0;
}
int __wrap_pd_primary_wait(const struct pd_primary* primary,const char* address,const struct pd_readiness* expected)
{
    event("wait");waited=1;assert(primary->pid==42 && !strcmp(address,"192.168.50.100") &&
        !strcmp(expected->hostname,"fixture") && !strcmp(expected->address,"100.64.0.1") && !strcmp(expected->dns,"fixture.example"));
    return strcmp(fault,"reject-readiness")!=0;
}
int __wrap_pd_primary_check(const struct pd_primary* primary)
{
    event("check");assert(primary->pid==42);return strcmp(fault,"reject-final-process")!=0;
}
void __wrap_pd_primary_close(struct pd_primary* primary)
{
    event("close");if(primary->process>=0){close(primary->process);}if(primary->executable>=0){close(primary->executable);}
    primary->process=primary->executable=-1;
}
int main(int argc,char** argv)
{
    if(argc!=5 || (strcmp(argv[2],"prepare") && strcmp(argv[2],"activate")) ||
        (strcmp(argv[3],"upgrade") && strcmp(argv[3],"installation"))){return 2;}
    root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    store=openat(root,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    upgrade=!strcmp(argv[3],"upgrade");fault=argv[4];
    unsigned char record[PD_UPGRADE_APPROVAL_SIZE];size_t size=upgrade?PD_UPGRADE_APPROVAL_SIZE:PD_MIGRATION_SIZE;
    int fd=openat(root,"approved",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0 || store<0 || fd<0 || read(fd,record,sizeof(record))!=(ssize_t)size){return 2;}close(fd);
    if(strcmp(fault,"no-worker")){
        lease=openat(store,"transaction.lock",O_RDWR|O_NOFOLLOW|O_CLOEXEC);
        if(lease<0 || flock(lease,LOCK_EX|LOCK_NB)){return 2;}
    }
    int ok=0;
    if(!strcmp(argv[2],"prepare")){
        ok=upgrade?pd_service_upgrade_prepare_runtime(store,record):pd_service_prepare_runtime(store,record);
    }else{
        struct pd_readiness expected={"fixture","100.64.0.1","fixture.example"};
        struct pd_primary primary={.process=-1,.executable=-1};
        int executable=openat(root,"executable",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);assert(executable>=0);
        ok=upgrade?pd_service_upgrade_activate(store,record,executable,"192.168.50.100",&expected,&primary):
            pd_service_activate(store,record,executable,"192.168.50.100",&expected,&primary);
        if(ok){assert(primary.process>=0 && primary.executable>=0);__wrap_pd_primary_close(&primary);}
        else{assert(primary.process<0 && primary.executable<0);}
        close(executable);
    }
    if(lease>=0){close(lease);}close(store);close(root);return ok?0:1;
}
