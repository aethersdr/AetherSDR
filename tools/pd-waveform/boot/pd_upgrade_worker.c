#define _GNU_SOURCE
#include "pd_upgrade_capsule.h"
#include "pd_upgrade_state.h"
#include "pd_upgrade_units.h"
#include "pd_upgrade_bootstrap.h"
#include "pd_delivery_lease.h"
#include "pd_management_embedded.h"
#include "pd_release_key.h"
#include "pd_pair.h"
#include "pd_generation.h"
#include "pd_retained.h"
#include "pd_retire.h"
#include "pd_slots_retire.h"
#include "pd_bundle.h"
#include "pd_service.h"
#include "pd_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__aarch64__) || defined(PD_UPGRADE_WORKER_TEST)
static const uint32_t kArch=64;
static const char* const kParent[]={"var","lib","smartsdr"};
#elif defined(__arm__)
static const uint32_t kArch=7;
static const char* const kParent[]={"user"};
#else
static const uint32_t kArch=0;
static const char* const kParent[]={"user"};
#endif
struct upgrade_worker {
    int parent,units,store,stage,control,generation;
    struct pd_upgrade_capsule capsule;
    struct pd_upgrade approved;
    struct pd_identity identity;
};
static int protected_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) &&
        st.st_uid==0 && !(st.st_mode&07022);
}
static int directory(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!protected_directory(fd)){if(fd>=0){close(fd);}return -1;}return fd;
}
static int path(int root,const char* const* names,size_t count)
{
    int fd=fcntl(root,F_DUPFD_CLOEXEC,3);
    for(size_t i=0;fd>=0 && i<count;++i){int next=directory(fd,names[i]);close(fd);fd=next;}
    return fd;
}
static int target(struct upgrade_worker* worker)
{
    const struct pd_approval* approved=&worker->capsule.approval.candidate;
    return pd_identity_probe(&worker->identity) && pd_identity_matches(&worker->identity,
        approved->profile,approved->firmware,approved->serial);
}
static int reopen_previous(struct upgrade_worker* worker)
{
    return target(worker) && pd_upgrade_reopen_choice(worker->stage,pd_release_key,kArch,
        &worker->identity,PD_PAIR_PREVIOUS,&worker->approved) &&
        !memcmp(worker->approved.record,worker->capsule.approval.record,PD_UPGRADE_APPROVAL_SIZE);
}
static int manage(struct upgrade_worker* worker)
{
    if(!reopen_previous(worker)){fputs("AetherPD upgrade: previous release verification failed\n",stderr);return 0;}
    int applied=worker->capsule.intent_size==PD_UPGRADE_CAPSULE_ORIGIN_SIZE?
        pd_management_apply_origin(worker->store,worker->units,&worker->approved,&pd_management_embedded,&worker->capsule.origin):
        pd_management_apply(worker->store,worker->units,&worker->approved,&pd_management_embedded);
    if(!applied){
        fputs("AetherPD upgrade: management file transition failed\n",stderr);return 0;
    }
    if(!pd_service_reload()){fputs("AetherPD upgrade: service reload verification failed\n",stderr);return 0;}
    return 1;
}
static int quiesce(void* context)
{
    struct upgrade_worker* worker=context;
    return target(worker) && pd_runtime_upgrade_retire(worker->store,worker->capsule.approval.record) && manage(worker) &&
        (!worker->generation || pd_generation_select(worker->store,&worker->approved,&worker->capsule.origin,kArch,&worker->identity));
}
static int select_candidate(void* context)
{
    struct upgrade_worker* worker=context;
    return target(worker) && pd_pair_publish(worker->control,worker->stage,pd_release_key,kArch,
        &worker->identity,PD_PAIR_CANDIDATE);
}
static int activate_choice(struct upgrade_worker* worker,unsigned choice)
{
    if(!target(worker) || !pd_service_upgrade_prepare_runtime(worker->store,worker->capsule.approval.record)){return 0;}
    struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};struct pd_bundle bundle;
    int ok=pd_pair_acquire(worker->control,worker->stage,pd_release_key,kArch,&worker->identity,&pair) && pair.choice==choice;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const char* const parts[]={"run","aetherpd","bin"};
    int bin=protected_directory(root)?path(root,parts,3):-1;if(root>=0){close(root);}
    ok=ok && bin>=0 && pd_bundle_read(pair.payload,pair.payload_key,kArch,&bundle) && pd_bundle_matches(bin,&bundle);
    int executable=ok?openat(bin,"flex-tailnet-shim",O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;
    struct pd_primary primary={.process=-1,.executable=-1};
    ok=ok && executable>=0 && pd_service_upgrade_activate(worker->store,worker->capsule.approval.record,
        executable,worker->identity.address,&worker->capsule.expected,&primary);
    pd_primary_close(&primary);if(executable>=0){close(executable);}if(bin>=0){close(bin);}pd_pair_close(&pair);
    return ok && pd_service_primary_autostart(worker->units,1);
}
static int activate(void* context){return activate_choice(context,PD_PAIR_CANDIDATE);}
static int rollback(void* context)
{
    struct upgrade_worker* worker=context;
    if(!pd_service_runtime_quiet() && !pd_service_quiesce()){return 0;}
    /* A crashed replacement may leave mixed service definitions. Repair the
     * authenticated management files before asking systemd to reload them.
     * Runtime quiescence is rechecked before management repair. */
    if(!manage(worker)){return 0;}
    if(!pd_runtime_upgrade_retire(worker->store,worker->capsule.approval.record)){
        fputs("AetherPD upgrade: rollback runtime retirement failed\n",stderr);return 0;
    }
    return target(worker) && pd_pair_publish(worker->control,worker->stage,pd_release_key,kArch,
        &worker->identity,PD_PAIR_PREVIOUS) &&
        (!worker->generation || pd_generation_select(worker->store,&worker->approved,&worker->capsule.origin,kArch,&worker->identity));
}
static int restore(void* context){return activate_choice(context,PD_PAIR_PREVIOUS);}
static int cleanup(void* context)
{
    struct upgrade_worker* worker=context;
    if(worker->generation){
        if(!pd_slots_retire(worker->store,pd_release_key,kArch,&worker->identity)){
            fputs("AetherPD upgrade: original slot retirement incomplete\n",stderr);
        }
        if(!pd_retained_compact(worker->store,pd_release_key,kArch,&worker->identity)){
            fputs("AetherPD upgrade: inactive payload retained; storage compaction incomplete\n",stderr);
        }else if(!pd_retire_predecessor(worker->store,pd_release_key,kArch,&worker->identity,&worker->capsule.origin)){
            fputs("AetherPD upgrade: predecessor retirement incomplete\n",stderr);
        }
    }
    if(!pd_upgrade_capsule_retire(&worker->capsule,worker->store)){return 0;}
    pd_upgrade_capsule_close(&worker->capsule);
    return pd_upgrade_finish_cleanup(worker->parent,worker->units,worker->store,pd_release_key);
}
static int origin_supported(struct upgrade_worker* worker)
{
    struct pd_release candidate;const struct pd_upgrade* approval=&worker->capsule.approval;
    if(!pd_release_header_parse(pd_management_embedded.header,sizeof(pd_management_embedded.header),
        approval->candidate_key,kArch,approval->candidate.profile,approval->candidate.firmware,&candidate)){return 0;}
    worker->generation=candidate.generation_selection;
    return worker->capsule.intent_size==PD_UPGRADE_CAPSULE_SIZE?!worker->generation:candidate.origin_recovery;
}
static int execute(struct upgrade_worker* worker,int recovery)
{
    int self=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
    int admitted=pd_upgrade_capsule_self(&worker->capsule,self) &&
        !memcmp(worker->capsule.approval.candidate.digest,pd_management_embedded.release_digest,32) &&
        pd_service_upgrade_worker_ready(worker->units,recovery) &&
        origin_supported(worker) && pd_upgrade_worker_admit_predecessor(worker->store,worker->capsule.approval.record,
            worker->generation && worker->capsule.origin.has_predecessor?&worker->capsule.origin.predecessor:NULL);
    if(self>=0){close(self);}if(!admitted){return 3;}
    if(!target(worker)){return 4;}
    struct pd_upgrade_state state;int present=pd_upgrade_state_read(worker->store,&state);
    if(present<0){return 3;}
    int same=present && !memcmp(state.approval.record,worker->capsule.approval.record,PD_UPGRADE_APPROVAL_SIZE);
    int terminal=same && (state.phase==PD_UPGRADE_COMMITTED || state.phase==PD_UPGRADE_ROLLED_BACK);
    if(!terminal){
        if(worker->generation){
            struct pd_generation generation;
            if(!pd_generation_open(worker->store,&worker->capsule.approval,0,&generation)){return 3;}
            worker->stage=generation.stage;worker->control=generation.control;generation.stage=generation.control=-1;
            pd_generation_close(&generation);
        }else{
            worker->stage=directory(worker->store,"upgrade-pair");worker->control=directory(worker->store,"upgrade-control");
        }
        if(worker->stage<0 || worker->control<0 || !reopen_previous(worker)){return 3;}
        struct pd_upgrade both;
        if(!recovery && !pd_upgrade_reopen(worker->stage,pd_release_key,kArch,&worker->identity,&both)){recovery=1;}
        int first=pd_upgrade_capsule_begin(&worker->capsule);
        if(first<0){return 4;}if(!first){recovery=1;}
        /* Existing sessions hold shared transaction leases. Stop their fixed
         * service graph before requesting the exclusive lease. Mixed files are
         * repaired inside rollback only when no runtime process remains. */
        if(!pd_service_runtime_quiet() && !pd_service_quiesce()){return 4;}
    }
    struct pd_upgrade_ops ops={worker,quiesce,select_candidate,activate,rollback,restore,cleanup};
    int result=pd_upgrade_transaction(worker->store,worker->capsule.approval.record,&ops,recovery);
    return result>=0?0:4;
}
int main(int argc,char** argv)
{
    int recovery=argc==2 && !strcmp(argv[1],"--recover");
    int delivered=argc==2 && !strcmp(argv[1],"--delivered");
    int bootstrap=delivered || (argc==2 && !strcmp(argv[1],"--bootstrap"));
    if((argc!=1 && !recovery && !bootstrap) || getuid()!=0 || geteuid()!=0 || !kArch){return 2;}
    if(delivered && !pd_delivery_lease_adopt()){return 3;}
    umask(077);int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!protected_directory(root)){if(root>=0){close(root);}return 3;}
    const char* const unit_parts[]={"etc","systemd","system"};
    struct upgrade_worker worker={.parent=-1,.units=-1,.store=-1,.stage=-1,.control=-1};
    worker.parent=path(root,kParent,sizeof(kParent)/sizeof(kParent[0]));worker.units=path(root,unit_parts,3);
    worker.store=worker.parent>=0?directory(worker.parent,"aetherpd"):-1;int result=3;
    if(worker.parent>=0 && worker.units>=0 && worker.store>=0){
        if(bootstrap){
            int self=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
            result=pd_upgrade_bootstrap(root,worker.parent,worker.units,worker.store,self,kArch,pd_release_key,&pd_management_embedded);
            if(self>=0){close(self);}
        }else if(pd_upgrade_capsule_open(worker.parent,pd_release_key,&worker.capsule)){
            result=execute(&worker,recovery);pd_upgrade_capsule_close(&worker.capsule);
        }else if(errno==EWOULDBLOCK){result=4;}
    }
    close(root);
    if(worker.control>=0){close(worker.control);}if(worker.stage>=0){close(worker.stage);}
    if(worker.store>=0){close(worker.store);}if(worker.units>=0){close(worker.units);}if(worker.parent>=0){close(worker.parent);}
    return result;
}
