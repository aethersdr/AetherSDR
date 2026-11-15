#define PD_INSTALL_MAIN inherited_install_main
#include "pd_install_release.c"
#undef PD_INSTALL_MAIN
#include "pd_capsule.h"
#include "pd_transaction.h"
#include "pd_worker_units.h"
#include "pd_runtime.h"
#include "pd_service.h"
#include "pd_recovery.h"
#include "pd_readiness.h"
#include "pd_bundle.h"
#include "pd_store.h"
#include "monocypher.h"
#include <sys/file.h>
#include "pd_delivery_lease.h"

#if defined(__aarch64__) || defined(PD_WORKER_TEST)
struct worker {
    int parent,units,store;
    struct pd_capsule capsule;
    struct pd_identity identity;
    unsigned char plan[PD_MIGRATION_SIZE];
};
static int worker_target(struct worker* worker)
{
    return pd_identity_probe(&worker->identity) && pd_identity_matches(&worker->identity,
        worker->capsule.approval.profile,worker->capsule.approval.firmware,worker->capsule.approval.serial);
}
static int worker_stage_approval(int stage,const unsigned char expected[PD_APPROVAL_SIZE])
{
    int file=openat(stage,"approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char bytes[PD_APPROVAL_SIZE];struct pd_approval parsed;
    int ok=pd_approval_read(file,bytes,&parsed) && !memcmp(bytes,expected,sizeof(bytes));
    if(file>=0){close(file);}return ok;
}
static int worker_prepare(void* context)
{
    struct worker* worker=context;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const char* const parts[]={"run","aetherpd-install"};
    int stage=root>=0?install_path(root,parts,2):-1;if(root>=0){close(root);}
    struct pd_predecessor observed;
    int ok=stage>=0 && worker_stage_approval(stage,worker->capsule.migration.approval) && worker_target(worker) && pd_predecessor_probe(&observed) &&
        pd_migration_matches(&worker->capsule.migration,&observed) && pd_predecessor_image_verify(&observed) &&
        pd_install_prepare(worker->parent,worker->units,stage,pd_release_key,&worker->capsule.artifact,worker->capsule.approval.digest) &&
        pd_service_reload();
    if(stage>=0){close(stage);}return ok;
}
static int worker_copy(void* context)
{
    struct worker* worker=context;return worker_target(worker) && pd_migration_copy(worker->store,worker->plan)==1;
}
static int worker_retire(void* context)
{
    struct worker* worker=context;return worker_target(worker) && pd_predecessor_retire(worker->store,worker->plan);
}
static int worker_activate(void* context)
{
    struct worker* worker=context;
    if(!worker_target(worker) || !pd_service_prepare_runtime(worker->store,worker->plan)){return 0;}
    struct pd_selection selection;struct pd_bundle bundle;struct pd_readiness expected;unsigned char digest[32];
    int ok=pd_selection_read(worker->store,&selection)==1;
    int slots=ok?openat(worker->store,"slots",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int payload=slots>=0?pd_store_slot(slots,selection.current):-1;if(slots>=0){close(slots);}
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const char* const parts[]={"run","aetherpd","bin"};
    int runtime=root>=0?install_path(root,parts,3):-1;if(root>=0){close(root);}
    ok=ok && payload>=0 && runtime>=0 && pd_bundle_hash(payload,digest) &&
        !crypto_verify32(digest,selection.current_hash) &&
        !crypto_verify32(digest,worker->capsule.artifact.manifest+PD_RELEASE_HEADER+2*PD_RELEASE_RECORD+4) &&
        pd_bundle_read(payload,pd_release_key,64,&bundle) && pd_bundle_matches(runtime,&bundle) &&
        pd_readiness_read(worker->store,worker->plan,&expected);
    int executable=ok?openat(runtime,"flex-tailnet-shim",O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;
    struct pd_primary primary={.process=-1,.executable=-1};
    ok=ok && executable>=0 && pd_service_activate(worker->store,worker->plan,executable,worker->identity.address,&expected,&primary);
    pd_primary_close(&primary);if(executable>=0){close(executable);}if(runtime>=0){close(runtime);}
    if(payload>=0){close(payload);}return ok && pd_service_primary_autostart(worker->units,1);
}
static int worker_rollback(void* context)
{
    struct worker* worker=context;
    if(!worker_target(worker) || !pd_service_primary_autostart(worker->units,0) || !pd_runtime_retire(worker->store,worker->plan)){return 0;}
    /* Before the first stop, no recovery recipe may exist. Accept only the
     * still-running approved predecessor in that case; never create one. */
    struct pd_predecessor observed;struct pd_readiness expected;struct stat st;
    int missing=fstatat(worker->store,"recovery-recipe",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    if(missing){
        return pd_predecessor_probe(&observed) && pd_migration_matches(&worker->capsule.migration,&observed) &&
            pd_predecessor_image_verify(&observed) && pd_readiness_probe(worker->identity.address,&expected);
    }
    if(!pd_readiness_read(worker->store,worker->plan,&expected)){return 0;}
    if(pd_migration_recover(worker->store,worker->plan)==1){return 1;}
    return pd_predecessor_restore(worker->store,worker->plan) && pd_readiness_wait(worker->identity.address,&expected);
}
static int worker_cleanup(void* context)
{
    struct worker* worker=context;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);const char* const parts[]={"run"};
    int run=root>=0?install_path(root,parts,1):-1;if(root>=0){close(root);}
    int discarded=run>=0 && pd_install_stage_discard(run,&worker->capsule.artifact,worker->plan+8);
    if(run>=0){close(run);}
    if(!discarded || !pd_capsule_retire(&worker->capsule,worker->store)){return 0;}
    pd_capsule_close(&worker->capsule);
    return pd_capsule_finish_cleanup(worker->parent,worker->units,worker->store,pd_release_key,worker->plan);
}

static int worker_rearm(int parent,int units,int self,const unsigned char plan[PD_MIGRATION_SIZE])
{
    struct pd_capsule capsule;if(!pd_capsule_open(parent,pd_release_key,&capsule)){return 4;}
    unsigned char digest[32];
    int ok=!memcmp(capsule.intent+72,plan,PD_MIGRATION_SIZE) && pd_bundle_hash(self,digest) &&
        !crypto_verify32(digest,capsule.intent+40) && pd_worker_units_arm(units) && pd_service_worker_launch(units);
    pd_capsule_close(&capsule);return ok?0:4;
}
static int worker_bootstrap_locked(int root,int parent,int units,int run,int self,
                                    const unsigned char target[PD_APPROVAL_SIZE],const unsigned char plan[PD_MIGRATION_SIZE],
                                    const struct pd_approval* approval,const struct pd_migration* migration,const struct pd_release* release)
{
    struct stat st;
    if(!fstatat(parent,"aetherpd-installation",&st,AT_SYMLINK_NOFOLLOW)){
        return worker_rearm(parent,units,self,plan);
    }
    if(errno!=ENOENT){return 4;}
    int store=openat(parent,"aetherpd",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(store>=0){
        int terminal=pd_transaction_at_phase(store,plan,PD_TRANSACTION_COMMITTED) || pd_transaction_at_phase(store,plan,PD_TRANSACTION_ROLLED_BACK);
        if(terminal){
            int ok=pd_capsule_finish_cleanup(parent,units,store,pd_release_key,plan);close(store);return ok?0:4;
        }
        close(store);
    }else if(errno!=ENOENT){return 4;}
    struct pd_predecessor observed;
    if(!pd_predecessor_probe(&observed) || !pd_migration_matches(migration,&observed) || !pd_predecessor_image_verify(&observed)){return 3;}
    char* stage_args[]={"aetherpd-stage",NULL};
    if(fstatat(run,"aetherpd-install",&st,AT_SYMLINK_NOFOLLOW)){
        if(errno!=ENOENT || inherited_stage_main(1,stage_args)){return 4;}
    }else if(!protected_directory(&st)){return 3;}
    const char* const stage_parts[]={"run","aetherpd-install"};
    int stage=install_path(root,stage_parts,2);
    int ok=stage>=0 && worker_stage_approval(stage,target) && pd_install_prepare(parent,units,stage,pd_release_key,release,approval->digest);
    if(stage>=0){close(stage);}
    store=ok?openat(parent,"aetherpd",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    ok=ok && store>=0 && pd_migration_bind(store,plan) && pd_capsule_publish(parent,self,0,pd_release_key,plan);
    if(store>=0){close(store);}return ok?worker_rearm(parent,units,self,plan):4;
}
static int worker_bootstrap(void)
{
    unsigned char target[PD_APPROVAL_SIZE],plan[PD_MIGRATION_SIZE];
    struct pd_approval approval;struct pd_migration migration;struct pd_release release;struct pd_identity identity;
    if(!pd_approval_read(3,target,&approval) || !pd_migration_read(4,plan,&migration) ||
        memcmp(target,migration.approval,sizeof(target)) ||
        !pd_release_read(0,pd_release_key,64,approval.profile,approval.firmware,approval.digest,&release)){return 3;}
    if(!pd_identity_probe(&identity)){return 4;}
    if(!pd_identity_matches(&identity,approval.profile,approval.firmware,approval.serial)){return 3;}
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 3;}
    const char* const parent_parts[]={"var","lib","smartsdr"},*const unit_parts[]={"etc","systemd","system"},*const run_parts[]={"run"};
    int parent=install_path(root,parent_parts,3),units=install_path(root,unit_parts,3),run=install_path(root,run_parts,1);
    int self=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
    int ok=parent>=0 && units>=0 && run>=0 && self>=0 && !fstat(self,&st) && S_ISREG(st.st_mode) && st.st_uid==0 &&
        st.st_nlink==1 && !(st.st_mode&07022) && st.st_size>0 && st.st_size<=PD_CAPSULE_WORKER_MAX;
    int lock=ok?openat(run,".aetherpd-bootstrap.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600):-1;
    ok=ok && lock>=0 && !fstat(lock,&st) && S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 &&
        (st.st_mode&07777)==0600 && st.st_size==0 && !flock(lock,LOCK_EX|LOCK_NB);
    int result=ok?worker_bootstrap_locked(root,parent,units,run,self,target,plan,&approval,&migration,&release):4;
    if(lock>=0){close(lock);}if(self>=0){close(self);}if(run>=0){close(run);}if(units>=0){close(units);}
    if(parent>=0){close(parent);}close(root);return result;
}

#endif
int main(int argc,char** argv)
{
    int recovery=argc==2 && !strcmp(argv[1],"--recover");
    int delivered=argc==2 && !strcmp(argv[1],"--delivered");
    int bootstrap=delivered || (argc==2 && !strcmp(argv[1],"--bootstrap"));
    if((argc!=1 && !recovery && !bootstrap) || getuid()!=0 || geteuid()!=0){return 2;}
    if(delivered && !pd_delivery_lease_adopt()){return 3;}
#if !defined(__aarch64__) && !defined(PD_WORKER_TEST)
    return 2;
#else
    umask(077);if(bootstrap){return worker_bootstrap();}
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 3;}
    const char* const parent_parts[]={"var","lib","smartsdr"};const char* const unit_parts[]={"etc","systemd","system"};
    struct worker worker={.parent=-1,.units=-1,.store=-1};
    worker.parent=install_path(root,parent_parts,3);worker.units=install_path(root,unit_parts,3);close(root);
    worker.store=worker.parent>=0?openat(worker.parent,"aetherpd",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int result=3,opened=0;
    if(worker.parent>=0 && worker.units>=0 && worker.store>=0){
        opened=pd_capsule_open(worker.parent,pd_release_key,&worker.capsule);
        if(!opened && errno==EWOULDBLOCK){result=4;}
    }
    if(opened){
        int self=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
        int admitted=pd_capsule_self(&worker.capsule,self);
        if(admitted && !pd_identity_probe(&worker.identity)){admitted=0;result=4;}
        admitted=admitted && pd_identity_matches(&worker.identity,worker.capsule.approval.profile,
            worker.capsule.approval.firmware,worker.capsule.approval.serial) && pd_service_worker_ready(worker.units,recovery);
        if(self>=0){close(self);}memcpy(worker.plan,worker.capsule.intent+72,sizeof(worker.plan));
        if(admitted){
            struct pd_transaction_ops ops={&worker,worker_prepare,worker_copy,worker_retire,worker_activate,worker_rollback,worker_cleanup};
            int transaction=recovery?pd_transaction_recover(worker.store,worker.plan,&ops):pd_transaction_run(worker.store,worker.plan,&ops);
            result=transaction>=0?0:4;
        }
        pd_capsule_close(&worker.capsule);
    }
    if(worker.store>=0){close(worker.store);}if(worker.units>=0){close(worker.units);}if(worker.parent>=0){close(worker.parent);}
    return result;
#endif
}
