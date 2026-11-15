#define _GNU_SOURCE
/* Isolated test hardware adapters. Never linked into a radio artifact. */
#include "pd_identity.h"
#include "pd_migration.h"
#include "pd_readiness.h"
#include "pd_service.h"
#include "pd_transaction.h"
#include "pd_bundle.h"
#include "pd_store.h"
#include "pd_release_key.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
struct model {int old_present,old_running,new_running,enabled,launched;};
static int load(struct model* model)
{
    int fd=open("/fixture/model",O_RDONLY|O_CLOEXEC);
    int ok=fd>=0 && read(fd,model,sizeof(*model))==sizeof(*model);if(fd>=0){close(fd);}return ok;
}
static int save(const struct model* model)
{
    int fd=open("/fixture/model",O_WRONLY|O_TRUNC|O_CLOEXEC);
    int ok=fd>=0 && write(fd,model,sizeof(*model))==sizeof(*model) && !fsync(fd);if(fd>=0){close(fd);}return ok;
}
static int point(const char* action)
{
    int log=open("/fixture/events",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(log<0){return 0;}int ok=dprintf(log,"%s\n",action)>0 && !fsync(log);close(log);
    char fault[128]={0};int fd=open("/fixture/fault",O_RDONLY|O_CLOEXEC);
    if(fd>=0){ssize_t n=read(fd,fault,sizeof(fault)-1);close(fd);if(n<0){return 0;}}
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,action)){raise(SIGKILL);_exit(90);}
    return ok && (strncmp(fault,"fail:",5) || strcmp(fault+5,action));
}
static int file_copy(const char* from,const char* to)
{
    char bytes[65536];int input=open(from,O_RDONLY|O_CLOEXEC);ssize_t n=input>=0?read(input,bytes,sizeof(bytes)):-1;
    if(input>=0){close(input);}if(n<0 || n==sizeof(bytes)){return 0;}
    int output=open(to,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    int ok=output>=0 && write(output,bytes,(size_t)n)==n && !fsync(output);if(output>=0){close(output);}return ok;
}
static int identity_copy(void)
{
    return file_copy("/var/lib/smartsdr/aetherpd/state/provision.json","/fixture/old/provision.json") &&
        file_copy("/var/lib/smartsdr/aetherpd/state/tsnet/tailscaled.state","/fixture/old/tsnet/tailscaled.state");
}
int pd_stage_checkpoint(const char* p){char name[100];snprintf(name,sizeof(name),"stage:%s",p);return point(name);}
int pd_install_checkpoint(const char* p){char name[100];snprintf(name,sizeof(name),"install:%s",p);return point(name);}
int pd_store_checkpoint(const char* p){(void)p;return 1;}
uint64_t pd_store_test_space(uint64_t space){return space;}
int pd_capsule_checkpoint(const char* p){char name[100];snprintf(name,sizeof(name),"capsule:%s",p);return point(name);}
uint64_t pd_capsule_test_space(uint64_t space){return space;}
int pd_transaction_checkpoint(const char* p,unsigned phase){char name[100];snprintf(name,sizeof(name),"transaction:%s:%u",p,phase);return point(name);}
int pd_runtime_test_root(void){return open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);}
int pd_runtime_test_quiet(void)
{
    struct model model;if(!load(&model) || !point("quiesce")){return 0;}model.new_running=0;return save(&model);
}
int pd_runtime_checkpoint(void){return point("runtime:unlink");}
int __wrap_pd_identity_probe(struct pd_identity* identity)
{
    memset(identity,0,sizeof(*identity));strcpy(identity->model,"FLEX-8600");strcpy(identity->serial,"fixture-8600");
    strcpy(identity->firmware,"4.3.16.42034");strcpy(identity->address,"192.168.50.100");return point("identity");
}
int __wrap_pd_predecessor_probe(struct pd_predecessor* observed)
{
    struct model model;if(!load(&model) || !model.old_present || !model.old_running){return 0;}
    memset(observed,0,sizeof(*observed));memset(observed->container,'a',64);memset(observed->image,'b',64);
    for(unsigned i=0;i<32;++i){observed->executable[i]=(unsigned char)i;}
    strcpy(observed->radio_address,"172.30.2.1");strcpy(observed->hostname,"fixture");observed->pid=42;
    return point("predecessor:probe");
}
int __wrap_pd_predecessor_image_verify(const struct pd_predecessor* p){(void)p;return point("predecessor:image");}
int __wrap_pd_predecessor_pin(const struct pd_predecessor* p,int* root,int* state,int* ts)
{
    (void)p;*root=open("/fixture/old",O_RDONLY|O_DIRECTORY|O_CLOEXEC);*state=dup(*root);*ts=openat(*root,"tsnet",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    return *root>=0 && *state>=0 && *ts>=0;
}
int __wrap_pd_predecessor_stop(const struct pd_predecessor* p,int root)
{
    (void)p;(void)root;struct model model;if(!load(&model) || model.new_running || !point("stop:before")){return 0;}
    model.old_running=0;return save(&model) && point("stop:after");
}
int __wrap_pd_predecessor_stopped(const struct pd_predecessor* p){(void)p;struct model m;return load(&m) && m.old_present && !m.old_running;}
int __wrap_pd_predecessor_recover_fresh(const struct pd_predecessor* p)
{
    (void)p;struct model model;if(!load(&model) || !model.old_present || model.new_running || !point("recover:before")){return 0;}
    model.old_running=1;return save(&model) && point("recover:after");
}
int __wrap_pd_predecessor_recover(const struct pd_predecessor* p,int root){(void)root;return __wrap_pd_predecessor_recover_fresh(p);}
int __wrap_pd_predecessor_retire(int store,const unsigned char* plan)
{
    struct model model;
    if(!pd_transaction_at_phase(store,plan,PD_TRANSACTION_RETIRING) || !load(&model) || model.old_running || model.new_running || !point("retire:before")){return 0;}
    model.old_present=0;return save(&model) && point("retire:after");
}
int __wrap_pd_predecessor_restore(int store,const unsigned char* plan)
{
    struct model model;
    if(!pd_transaction_at_phase(store,plan,PD_TRANSACTION_ROLLBACK) || !load(&model) || model.new_running || !point("restore:before") || !identity_copy()){return 0;}
    model.old_present=model.old_running=1;return save(&model) && point("restore:after");
}
int __wrap_pd_readiness_probe(const char* address,struct pd_readiness* ready)
{
    (void)address;struct model model;if(!load(&model) || model.new_running==model.old_running){return 0;}
    memset(ready,0,sizeof(*ready));strcpy(ready->hostname,"fixture");strcpy(ready->address,"100.64.1.2");strcpy(ready->dns,"fixture.example.ts.net");return point("readiness");
}
int __wrap_pd_readiness_wait(const char* address,const struct pd_readiness* expected)
{
    struct pd_readiness actual;return __wrap_pd_readiness_probe(address,&actual) && pd_readiness_equal(&actual,expected);
}
int __wrap_pd_service_reload(void){return point("service:reload");}
int __wrap_pd_service_worker_ready(int units,int recovery)
{
    (void)units;(void)recovery;struct model model;return load(&model) && model.launched && point("service:worker-admission");
}
int __wrap_pd_service_worker_launch(int units)
{
    (void)units;struct model model;if(!load(&model) || !point("launch:before")){return 0;}
    model.launched=1;return save(&model) && point("launch:after");
}
int __wrap_pd_service_prepare_runtime(int store,const unsigned char* plan)
{
    if(!pd_transaction_at_phase(store,plan,PD_TRANSACTION_ACTIVATING) || !point("runtime:before")){return 0;}
    struct pd_selection selected;struct pd_bundle bundle;
    int ok=pd_selection_read(store,&selected)==1;
    int slots=ok?openat(store,"slots",O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;
    int payload=slots>=0?pd_store_slot(slots,selected.current):-1;if(slots>=0){close(slots);}
    ok=ok && payload>=0 && pd_bundle_read(payload,pd_release_key,64,&bundle) && !mkdir("/run/aetherpd",0755) && !mkdir("/run/aetherpd/bin",0755);
    int bin=ok?open("/run/aetherpd/bin",O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;
    ok=ok && bin>=0 && pd_bundle_extract(payload,bin,&bundle);
    if(bin>=0){close(bin);}if(payload>=0){close(payload);}return ok && point("runtime:after");
}
int __wrap_pd_service_activate(int store,const unsigned char* plan,int executable,const char* address,const struct pd_readiness* expected,struct pd_primary* primary)
{
    (void)executable;(void)address;(void)expected;(void)primary;struct model model;
    if(!pd_transaction_at_phase(store,plan,PD_TRANSACTION_ACTIVATING) || !load(&model) || model.old_present || model.old_running || !point("activate:before")){return 0;}
    model.new_running=1;
    int fd=openat(store,"state/tsnet/tailscaled.state",O_WRONLY|O_TRUNC|O_CLOEXEC);
    int changed=fd>=0 && write(fd,"rotated-identity",16)==16 && !fsync(fd);if(fd>=0){close(fd);}
    return changed && save(&model) && point("activate:after");
}
int __wrap_pd_service_primary_autostart(int units,int enabled)
{
    (void)units;struct model model;if(!load(&model) || !point(enabled?"enable:before":"disable:before")){return 0;}
    model.enabled=enabled;return save(&model) && point(enabled?"enable:after":"disable:after");
}
