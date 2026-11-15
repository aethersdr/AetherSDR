#define _GNU_SOURCE
/* Hardware adapters for the isolated worker test, never a radio build. */
#include "pd_pair.h"
#include "pd_service.h"
#include "pd_upgrade_state.h"
#include "pd_upgrade_units.h"
#include "pd_generation.h"
#include "pd_bundle.h"
#include "pd_release_key.h"
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static uint64_t allocated(const char* path,unsigned depth,uint64_t* inodes)
{
    struct stat st;if(lstat(path,&st)){return 0;}
    ++*inodes;
    uint64_t bytes=(uint64_t)st.st_blocks*512;
    if(!S_ISDIR(st.st_mode) || depth==8){return bytes;}
    DIR* dir=opendir(path);if(!dir){return bytes;}
    struct dirent* entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        char child[512];int length=snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);
        if(length>0 && (size_t)length<sizeof(child)){bytes+=allocated(child,depth+1,inodes);}
    }
    closedir(dir);return bytes;
}
static void measure_space(void)
{
    if(access("/fixture/measure-space",F_OK)){return;}
    uint64_t inodes=0;
    uint64_t bytes=allocated("/var/lib/smartsdr",0,&inodes)+allocated("/etc/systemd/system",0,&inodes);
    int fd=open("/fixture/space-observations",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd>=0){dprintf(fd,"%llu %llu\n",(unsigned long long)bytes,(unsigned long long)inodes);close(fd);}
}
void pd_upgrade_test_budget(uint64_t bytes,uint64_t inodes)
{
    if(access("/fixture/measure-space",F_OK)){return;}
    int fd=open("/fixture/space-budget",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd>=0){dprintf(fd,"%llu %llu\n",(unsigned long long)bytes,(unsigned long long)inodes);close(fd);}
}
static int point(const char* action)
{
    measure_space();
    int log=open("/fixture/events",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(log<0){return 0;}int ok=dprintf(log,"%s\n",action)>0 && !fsync(log);close(log);
    char fault[128]={0};int fd=open("/fixture/fault",O_RDONLY|O_CLOEXEC);
    if(fd>=0){ssize_t n=read(fd,fault,sizeof(fault)-1);close(fd);if(n<0){return 0;}}
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,action)){raise(SIGKILL);_exit(90);}
    return ok && (strncmp(fault,"fail:",5) || strcmp(fault+5,action));
}
static int running(void)
{
    int fd=open("/fixture/running",O_RDONLY|O_CLOEXEC);char value=0;
    int ok=fd>=0 && read(fd,&value,1)==1;if(fd>=0){close(fd);}return ok?value-'0':-1;
}
static int set_running(int value)
{
    char c=(char)('0'+value);int fd=open("/fixture/running",O_WRONLY|O_TRUNC|O_CLOEXEC);
    int ok=fd>=0 && write(fd,&c,1)==1 && !fsync(fd);if(fd>=0){close(fd);}return ok;
}
int pd_upgrade_checkpoint(const char* p,unsigned i){char n[128];snprintf(n,sizeof(n),"pair:%s:%u",p,i);return point(n);}
int pd_transaction_checkpoint(const char* p,unsigned i){char n[128];snprintf(n,sizeof(n),"transaction:%s:%u",p,i);return point(n);}
int pd_management_checkpoint(const char* p,unsigned i){char n[128];snprintf(n,sizeof(n),"management:%s:%u",p,i);return point(n);}
int pd_capsule_checkpoint(const char* p){return point(p);}
int pd_store_checkpoint(const char* p){(void)p;return 1;}
uint64_t pd_store_test_space(uint64_t space){return space;}
uint64_t pd_capsule_test_space(uint64_t space){return space;}
uint64_t pd_management_test_space(uint64_t space){return space;}
uint64_t pd_upgrade_test_free(uint64_t actual,int inodes)
{
    if(!point(inodes?"space:inodes":"space:bytes")){return 0;}
    int fd=open(inodes?"/fixture/free-inodes":"/fixture/free-bytes",O_RDONLY|O_CLOEXEC);
    if(fd<0){return actual;}char bytes[32]={0};ssize_t n=read(fd,bytes,sizeof(bytes)-1);close(fd);
    return n>0?strtoull(bytes,NULL,10):0;
}
int pd_runtime_test_root(void){return open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);}
int pd_runtime_test_quiet(void){return running()==0 && point("runtime:quiet");}
int pd_runtime_checkpoint(void){return point("runtime:unlink");}
int __wrap_pd_identity_probe(struct pd_identity* identity)
{
    memset(identity,0,sizeof(*identity));strcpy(identity->model,"FLEX-8600");strcpy(identity->serial,"fixture-8600");
    strcpy(identity->firmware,"4.3.16.42034");strcpy(identity->address,"192.168.50.100");return point("identity");
}
int __wrap_pd_service_runtime_quiet(void){return running()==0;}
int __wrap_pd_service_quiesce(void)
{
    return point("quiesce:before") && set_running(0) && point("quiesce:after");
}
int __wrap_pd_service_reload(void){return point("service:reload");}
int __wrap_pd_service_upgrade_cleanup_idle(void){return running()==1 && point("bootstrap:cleanup-idle");}
int __wrap_pd_service_upgrade_worker_ready(int units,int recovery)
{
    (void)units;(void)recovery;return !access("/fixture/launched",F_OK) && point("service:worker-admission");
}
int __wrap_pd_service_upgrade_worker_launch(int units)
{
    if(!pd_upgrade_units_verify(units) || !point("launch:before")){return 0;}
    int fd=open("/fixture/launched",O_WRONLY|O_CREAT|O_CLOEXEC,0600);
    int ok=fd>=0 && !fsync(fd);if(fd>=0){close(fd);}return ok && point("launch:after");
}
int __wrap_pd_service_read(unsigned kind,struct pd_service_state* state)
{
    memset(state,0,sizeof(*state));state->active=running()==1;state->pid=42;
    return kind==PD_SERVICE_PRIMARY && point("bootstrap:primary-status");
}
int __wrap_pd_readiness_probe(const char* address,struct pd_readiness* expected)
{
    if(strcmp(address,"192.168.50.100") || running()!=1){return 0;}
    memset(expected,0,sizeof(*expected));strcpy(expected->hostname,"fixture");strcpy(expected->address,"100.64.1.2");
    strcpy(expected->dns,"fixture.example.ts.net");return point("bootstrap:identity");
}
int __wrap_pd_primary_open(pid_t pid,int executable,struct pd_primary* primary)
{
    (void)primary;return pid==42 && executable>=0 && point("bootstrap:primary-open");
}
int __wrap_pd_primary_wait(const struct pd_primary* primary,const char* address,const struct pd_readiness* expected)
{
    (void)primary;return running()==1 && !strcmp(address,"192.168.50.100") &&
        !strcmp(expected->address,"100.64.1.2") && point("bootstrap:primary-wait");
}
int __wrap_pd_primary_check(const struct pd_primary* primary){(void)primary;return point("bootstrap:primary-check");}
void __wrap_pd_primary_close(struct pd_primary* primary){primary->process=primary->executable=-1;}
static int choice(int store,const unsigned char* approval)
{
    if(pd_upgrade_at_phase(store,approval,PD_UPGRADE_ACTIVATING)){return 1;}
    if(pd_upgrade_at_phase(store,approval,PD_UPGRADE_RESTORING)){return 0;}return -1;
}
int __wrap_pd_service_upgrade_prepare_runtime(int store,const unsigned char* approval)
{
    int selected=choice(store,approval);
    if(selected<0 || running()!=0 || !point(selected?"candidate:prepare":"previous:prepare")){return 0;}
    struct pd_generation generation;int active=pd_generation_active(store,&generation);
    int control=generation.control,stage=generation.stage;
    struct pd_identity identity;struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};struct pd_bundle payload;
    int ok=active==1 && control>=0 && stage>=0 && __wrap_pd_identity_probe(&identity) &&
        pd_pair_acquire(control,stage,pd_release_key,64,&identity,&pair) && pair.choice==(unsigned)selected &&
        pd_bundle_read(pair.payload,pair.payload_key,64,&payload) &&
        !mkdir("/run/aetherpd",0755) && !mkdir("/run/aetherpd/bin",0755);
    int bin=ok?open("/run/aetherpd/bin",O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;
    ok=ok && bin>=0 && pd_bundle_extract(pair.payload,bin,&payload);
    if(bin>=0){close(bin);}pd_pair_close(&pair);pd_generation_close(&generation);return ok;
}
int __wrap_pd_service_upgrade_activate(int store,const unsigned char* approval,int executable,
                                        const char* address,const struct pd_readiness* expected,struct pd_primary* primary)
{
    (void)primary;int selected=choice(store,approval);
    if(selected<0 || executable<0 || strcmp(address,"192.168.50.100") || strcmp(expected->hostname,"fixture") ||
        strcmp(expected->address,"100.64.1.2") || strcmp(expected->dns,"fixture.example.ts.net") ||
        !point(selected?"candidate:activate-before":"previous:activate-before")){return 0;}
    return set_running(selected+1) && point(selected?"candidate:activate-after":"previous:activate-after");
}
int __wrap_pd_service_primary_autostart(int units,int enabled)
{
    (void)units;return enabled==1 && point("autostart");
}
