#define _GNU_SOURCE
#include "pd_upgrade_bootstrap.h"
#include "pd_upgrade_capsule.h"
#include "pd_upgrade_units.h"
#include "pd_upgrade_state.h"
#include "pd_capsule_io.h"
#include "pd_pair.h"
#include "pd_generation.h"
#include "pd_service.h"
#include "pd_bundle.h"
#include "pd_store.h"
#include "pd_retained.h"
#include "pd_retire.h"
#include "pd_slots_retire.h"
#include "pd_upgrade_unit_text.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#ifdef PD_UPGRADE_TEST
extern uint64_t pd_upgrade_test_free(uint64_t actual,int inodes);
#else
static uint64_t pd_upgrade_test_free(uint64_t actual,int inodes){(void)inodes;return actual;}
#endif
#ifdef PD_UPGRADE_WORKER_TEST
extern void pd_upgrade_test_budget(uint64_t bytes,uint64_t inodes);
#else
static void pd_upgrade_test_budget(uint64_t bytes,uint64_t inodes){(void)bytes;(void)inodes;}
#endif

static int private_directory(int parent,const char* name)
{
    if(mkdirat(parent,name,0700) && errno!=EEXIST){return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat a,b;
    int ok=pd_capsule_io_directory(fd,1) && !fstat(parent,&a) && !fstat(fd,&b) && a.st_dev==b.st_dev && !fsync(parent);
    if(!ok){if(fd>=0){close(fd);}return -1;}return fd;
}
static int volatile_inputs(void)
{
    struct statfs previous,candidate;
    return !fstatfs(4,&previous) && !fstatfs(5,&candidate) &&
        (unsigned long)previous.f_type==0x01021994UL && (unsigned long)candidate.f_type==0x01021994UL;
}
struct volume {dev_t device;uint64_t block,bytes,inodes,available,free_inodes;int inode_known;};
static int volume(int fd,struct volume volumes[3],unsigned* count)
{
    struct stat st;struct statvfs fs;
    if(fstat(fd,&st) || fstatvfs(fd,&fs) || !fs.f_frsize || (uint64_t)fs.f_bavail>UINT64_MAX/fs.f_frsize){return -1;}
    for(unsigned i=0;i<*count;++i){if(volumes[i].device==st.st_dev){return (int)i;}}
    if(*count==3){return -1;}unsigned i=(*count)++;
    volumes[i]=(struct volume){st.st_dev,fs.f_frsize,PD_STORE_RESERVE,0,
        pd_upgrade_test_free((uint64_t)fs.f_bavail*fs.f_frsize,0),pd_upgrade_test_free(fs.f_favail,1),fs.f_files!=0};return (int)i;
}
static int allocation(struct volume* volume,uint64_t bytes)
{
    uint64_t block=volume->block;
    if(bytes>UINT64_MAX-(block-1)){return 0;}
    uint64_t rounded=((bytes+block-1)/block)*block;
    if(volume->bytes>UINT64_MAX-rounded || volume->inodes==UINT64_MAX){return 0;}
    volume->bytes+=rounded;++volume->inodes;return 1;
}
/* Charge all future durable allocations before staging: remaining signed
 * pair, full capsule, management temporaries, selectors/journals and units.
 * Existing partial capsule/management bytes receive no space credit. */
static int space(int parent,int units,int stage,const struct pd_upgrade* approved,uint64_t worker_size,uint32_t arch)
{
    struct volume volumes[3]={{0}};unsigned count=0;
    int p=volume(parent,volumes,&count),s=volume(stage,volumes,&count),u=volume(units,volumes,&count);
    if(p<0 || s<0 || u<0){return 0;}struct pd_upgrade_budget staged;
    if(!pd_upgrade_space(stage,&approved->previous_release,&approved->candidate_release,&staged) ||
        staged.required_bytes<PD_STORE_RESERVE ||
        volumes[s].bytes>UINT64_MAX-(staged.required_bytes-PD_STORE_RESERVE)){return 0;}
    volumes[s].bytes+=staged.required_bytes-PD_STORE_RESERVE;volumes[s].inodes+=staged.required_inodes;
    if(!allocation(&volumes[p],worker_size) || !allocation(&volumes[p],PD_UPGRADE_CAPSULE_ORIGIN_SIZE)){return 0;}
    const struct pd_release* release=&approved->candidate_release;
    if(!allocation(&volumes[s],release->length[0]) || !allocation(&volumes[s],release->length[3])){return 0;}
    for(unsigned i=4;i<PD_RELEASE_COUNT;++i){
        if(!allocation(&volumes[s],release->length[i]) || !allocation(&volumes[u],release->length[i])){return 0;}
    }
    const char* const* definitions=arch==7?pd_upgrade_unit_armv7:pd_upgrade_unit_arm64;
    for(unsigned i=0;i<2;++i){if(!allocation(&volumes[u],strlen(definitions[i]))){return 0;}}
    /* Capsule/control directories, lock files, pair and journal temporaries,
     * enablement links and directory growth. One block per small object. */
    for(unsigned i=0;i<16;++i){
        if(!allocation(&volumes[p],volumes[p].block) || !allocation(&volumes[s],volumes[s].block) ||
            !allocation(&volumes[u],volumes[u].block)){return 0;}
    }
    if(approved->candidate_release.generation_selection){
        if(!allocation(&volumes[s],PD_RETAINED_SIZE) || !allocation(&volumes[s],PD_RETIRE_RECORD_SIZE) ||
            !allocation(&volumes[s],PD_SLOTS_RETIRE_SIZE)){return 0;}
        /* Retained-receipt parent, approval directory and their parent growth.
         * Temporary receipts rename in place; do not credit existing partials. */
        for(unsigned i=0;i<4;++i){if(!allocation(&volumes[s],volumes[s].block)){return 0;}}
    }
    for(unsigned i=0;i<count;++i){
        pd_upgrade_test_budget(volumes[i].bytes,volumes[i].inodes);
        if(volumes[i].bytes>volumes[i].available ||
            (volumes[i].inode_known && volumes[i].inodes>volumes[i].free_inodes)){return 0;}
    }
    return 1;
}
static int public_identity(int root,int stage,int control,uint32_t arch,
                             const unsigned char key[32],const struct pd_identity* identity,struct pd_readiness* expected)
{
    struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};struct pd_bundle bundle;
    int ok=pd_pair_acquire(control,stage,key,arch,identity,&pair) && pair.choice==PD_PAIR_PREVIOUS;
    const char* const names[]={"run","aetherpd","bin"};int bin=fcntl(root,F_DUPFD_CLOEXEC,6);
    for(unsigned i=0;bin>=0 && i<3;++i){
        int next=openat(bin,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(bin);bin=next;
        if(!pd_capsule_io_directory(bin,0)){if(bin>=0){close(bin);}bin=-1;}
    }
    ok=ok && bin>=0 && pd_bundle_read(pair.payload,pair.payload_key,arch,&bundle) && pd_bundle_matches(bin,&bundle);
    int executable=ok?openat(bin,"flex-tailnet-shim",O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;
    struct pd_service_state state={0};struct pd_primary primary={.process=-1,.executable=-1};
    ok=ok && executable>=0 && pd_service_files() && pd_service_read(PD_SERVICE_PRIMARY,&state) && state.active &&
        pd_primary_open(state.pid,executable,&primary) && pd_readiness_probe(identity->address,expected) &&
        pd_primary_wait(&primary,identity->address,expected) && pd_primary_check(&primary);
    pid_t pid=state.pid;
    ok=ok && pd_service_read(PD_SERVICE_PRIMARY,&state) && state.active && state.pid==pid;
    pd_primary_close(&primary);if(executable>=0){close(executable);}if(bin>=0){close(bin);}pd_pair_close(&pair);return ok;
}
static int rearm(int parent,int units,int self,const unsigned char key[32],const struct pd_upgrade* approved)
{
    struct pd_upgrade_capsule capsule;if(!pd_upgrade_capsule_open(parent,key,&capsule)){return 4;}
    unsigned char digest[32];off_t size=0;
    int ok=!memcmp(capsule.approval.record,approved->record,PD_UPGRADE_APPROVAL_SIZE) &&
        pd_capsule_io_hash(self,PD_UPGRADE_WORKER_MAX,digest,&size) &&
        !crypto_verify32(digest,capsule.intent+8+PD_UPGRADE_APPROVAL_SIZE) &&
        pd_upgrade_units_arm(units) && pd_service_upgrade_worker_launch(units);
    pd_upgrade_capsule_close(&capsule);return ok?0:4;
}
static int bootstrap_locked(int root,int parent,int units,int store,int self,uint32_t arch,
                              const unsigned char key[32],struct pd_upgrade* approved,uint64_t worker_size)
{
    struct pd_identity identity;
    if(!pd_identity_probe(&identity)){return 4;}
    if(!pd_identity_matches(&identity,approved->candidate.profile,approved->candidate.firmware,approved->candidate.serial)){return 3;}
    struct stat st;
    if(!fstatat(parent,"aetherpd-upgrade",&st,AT_SYMLINK_NOFOLLOW)){return rearm(parent,units,self,key,approved);}
    if(errno!=ENOENT){return 3;}
    struct pd_upgrade_state prior;int present=pd_upgrade_state_read(store,&prior);
    if(present<0){return 3;}
    if(present){
        if(prior.phase!=PD_UPGRADE_COMMITTED && prior.phase!=PD_UPGRADE_ROLLED_BACK){return 3;}
        if(!memcmp(prior.approval.record,approved->record,PD_UPGRADE_APPROVAL_SIZE)){
            return pd_upgrade_finish_cleanup(parent,units,store,key)?0:4;
        }
    }
    struct pd_upgrade_origin origin;
    if(!pd_upgrade_inspect_origin(store,units,3,4,5,key,arch,&identity,approved,&origin) || !approved->candidate_release.managed_startup ||
        !pd_service_files() || (present && !approved->candidate_release.generation_selection)){return 3;}
    if(approved->candidate_release.generation_selection && !volatile_inputs()){return 3;}
    int stage=-1,control=-1;
    if(approved->candidate_release.generation_selection){
        struct pd_generation generation;
        if(!pd_generation_open(store,approved,1,&generation)){return 4;}
        stage=generation.stage;control=generation.control;generation.stage=generation.control=-1;
        pd_generation_close(&generation);
    }else{
        stage=private_directory(store,"upgrade-pair");control=stage>=0?private_directory(store,"upgrade-control"):-1;
    }
    int ok=stage>=0 && control>=0 && space(parent,units,stage,approved,worker_size,arch) &&
        pd_upgrade_resume(stage,4,5,key,arch,&identity,approved) &&
        pd_pair_publish(control,stage,key,arch,&identity,PD_PAIR_PREVIOUS);
    struct pd_readiness expected;
    ok=ok && public_identity(root,stage,control,arch,key,&identity,&expected) &&
        pd_identity_probe(&identity) && pd_identity_matches(&identity,approved->candidate.profile,
            approved->candidate.firmware,approved->candidate.serial) &&
        (approved->candidate_release.generation_selection?
            pd_upgrade_capsule_publish_armed(parent,units,self,key,approved->record,&expected,&origin):
         approved->candidate_release.origin_recovery?
            pd_upgrade_capsule_publish_origin(parent,self,key,approved->record,&expected,&origin):
            pd_upgrade_capsule_publish(parent,self,key,approved->record,&expected));
    if(control>=0){close(control);}if(stage>=0){close(stage);}
    return ok?rearm(parent,units,self,key,approved):4;
}
int pd_upgrade_bootstrap(int root,int parent,int units,int store,int self,uint32_t arch,
                          const unsigned char key[32],const struct pd_management_blob* management)
{
    unsigned char bytes[PD_UPGRADE_APPROVAL_SIZE];struct pd_upgrade approved;struct stat st;
    if(!pd_capsule_io_file(3,sizeof(bytes),0600) || !pd_capsule_io_read(3,bytes,sizeof(bytes),0) ||
        !pd_upgrade_parse(bytes,sizeof(bytes),&approved) || memcmp(key,approved.candidate_key,32) ||
        memcmp(approved.candidate.digest,management->release_digest,32) ||
        fstat(self,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 ||
        (st.st_mode&07022) || st.st_size<=0 || st.st_size>PD_UPGRADE_WORKER_MAX){return 3;}
    uint64_t worker_size=(uint64_t)st.st_size;
    int lock=openat(parent,".aetherpd-upgrade-bootstrap.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(!pd_capsule_io_file(lock,0,0600) || flock(lock,LOCK_EX|LOCK_NB)){
        if(lock>=0){close(lock);}return 4;
    }
    int store_lock=pd_store_lock(store);
    int result=store_lock>=0?bootstrap_locked(root,parent,units,store,self,arch,key,&approved,worker_size):4;
    if(store_lock>=0){close(store_lock);}close(lock);return result;
}
