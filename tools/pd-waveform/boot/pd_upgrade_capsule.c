#define _GNU_SOURCE
#include "pd_upgrade_capsule.h"
#include "pd_capsule_io.h"
#include "pd_upgrade_state.h"
#include "pd_upgrade_units.h"
#include "pd_service.h"
#include "pd_store.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static const char kActive[]="aetherpd-upgrade";
static const char kPending[]=".aetherpd-upgrade";
static const char kStaging[]=".aetherpd-upgrade-staging";
static const char kRetired[]=".aetherpd-upgrade-retired";
enum { kWorkerHash=8+PD_UPGRADE_APPROVAL_SIZE, kIdentity=kWorkerHash+32 };
#ifdef PD_CAPSULE_FAULTS
extern int pd_capsule_checkpoint(const char* point);
extern uint64_t pd_capsule_test_space(uint64_t actual);
#else
static int pd_capsule_checkpoint(const char* point){(void)point;return 1;}
static uint64_t pd_capsule_test_space(uint64_t actual){return actual;}
#endif
static void initialize(struct pd_upgrade_capsule* value)
{
    memset(value,0,sizeof(*value));value->parent=value->lock=value->directory=value->worker=-1;
}
void pd_upgrade_capsule_close(struct pd_upgrade_capsule* value)
{
    if(value->worker>=0){close(value->worker);}if(value->directory>=0){close(value->directory);}
    if(value->parent>=0){close(value->parent);}if(value->lock>=0){close(value->lock);}
    value->parent=value->lock=value->directory=value->worker=-1;
}
static int lock_parent(int parent,int* guard)
{
    *guard=-1;if(!pd_capsule_io_directory(parent,0)){errno=EINVAL;return -1;}
    int directory=openat(parent,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!pd_capsule_io_directory(directory,0)){if(directory>=0){close(directory);}errno=EINVAL;return -1;}
    int lock=openat(directory,".aetherpd-upgrade.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(!pd_capsule_io_file(lock,0,0600)){
        if(lock>=0){close(lock);}close(directory);errno=EINVAL;return -1;
    }
    if(flock(lock,LOCK_EX|LOCK_NB) || fsync(lock)){
        int error=errno;close(lock);close(directory);errno=error;return -1;
    }
    *guard=lock;return directory;
}
static int inventory(int directory,int partial)
{
    const char* const names[]={"intent","worker","started","intent.new","worker.new"};
    int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;struct dirent* entry;
    for(;;){
        errno=0;entry=readdir(listing);if(!entry){ok=!errno;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        int known=0;for(unsigned i=0;i<(partial?5u:3u);++i){known|=!strcmp(entry->d_name,names[i]);}
        if(!known){ok=0;break;}
    }
    closedir(listing);return ok;
}
static int decode(const unsigned char* bytes,size_t size,const unsigned char key[32],
                    struct pd_upgrade* approval,struct pd_readiness* expected,struct pd_upgrade_origin* origin)
{
    int legacy=size==PD_UPGRADE_CAPSULE_SIZE && !memcmp(bytes,"PDUPCAP1",8);
    int extended=size==PD_UPGRADE_CAPSULE_ORIGIN_SIZE && !memcmp(bytes,"PDUPCAP2",8);
    if((!legacy && !extended) || !pd_upgrade_parse(bytes+8,PD_UPGRADE_APPROVAL_SIZE,approval) ||
        memcmp(key,approval->candidate_key,32)){return 0;}
    memset(expected,0,sizeof(*expected));memcpy(expected->hostname,bytes+kIdentity,64);
    memcpy(expected->address,bytes+kIdentity+64,16);memcpy(expected->dns,bytes+kIdentity+80,256);
    struct pd_upgrade_origin parsed;
    if(!pd_readiness_valid(expected) || (extended &&
        !pd_upgrade_origin_parse(bytes+PD_UPGRADE_CAPSULE_SIZE,PD_UPGRADE_ORIGIN_SIZE,approval,&parsed))){return 0;}
    if(origin){if(extended){*origin=parsed;}else{memset(origin,0,sizeof(*origin));}}
    return 1;
}
static void started_bytes(const unsigned char* intent,size_t size,unsigned char bytes[40])
{
    memcpy(bytes,"PDUPRUN1",8);crypto_blake2b(bytes+8,32,intent,size);
}
static int started_valid(int directory,const unsigned char* intent,size_t size)
{
    int fd=openat(directory,"started",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    struct stat st;unsigned char bytes[40],expected[40];started_bytes(intent,size,expected);
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=(off_t)sizeof(bytes) &&
        pd_capsule_io_file(fd,st.st_size,0600) && pd_capsule_io_read(fd,bytes,(size_t)st.st_size,0) &&
        !memcmp(bytes,expected,(size_t)st.st_size);close(fd);return ok;
}
static int validate(int directory,const unsigned char key[32],struct pd_upgrade_capsule* value)
{
    int fd=openat(directory,"intent",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    int ok=pd_capsule_io_directory(directory,1) && inventory(directory,0) && !fstat(fd,&st) &&
        (st.st_size==PD_UPGRADE_CAPSULE_SIZE || st.st_size==PD_UPGRADE_CAPSULE_ORIGIN_SIZE) &&
        pd_capsule_io_file(fd,st.st_size,0600);
    value->intent_size=ok?(size_t)st.st_size:0;
    ok=ok && pd_capsule_io_read(fd,value->intent,value->intent_size,0) &&
        decode(value->intent,value->intent_size,key,&value->approval,&value->expected,&value->origin) &&
        started_valid(directory,value->intent,value->intent_size);
    if(fd>=0){close(fd);}if(!ok){return 0;}
    value->worker=openat(directory,"worker",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char digest[32];off_t size=0;
    return pd_capsule_io_hash(value->worker,PD_UPGRADE_WORKER_MAX,digest,&size) &&
        pd_capsule_io_file(value->worker,size,0700) && !crypto_verify32(digest,value->intent+kWorkerHash);
}
int pd_upgrade_capsule_open(int parent,const unsigned char key[32],struct pd_upgrade_capsule* value)
{
    initialize(value);value->parent=lock_parent(parent,&value->lock);if(value->parent<0){return 0;}
    value->directory=openat(value->parent,kActive,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!validate(value->directory,key,value) || fsync(value->directory) || fsync(value->parent)){
        pd_upgrade_capsule_close(value);errno=EINVAL;return 0;
    }
    return 1;
}
int pd_upgrade_capsule_self(const struct pd_upgrade_capsule* value,int executable)
{
    struct stat running,stored;
    return executable>=0 && value->worker>=0 && !fstat(executable,&running) && !fstat(value->worker,&stored) &&
        S_ISREG(running.st_mode) && running.st_dev==stored.st_dev && running.st_ino==stored.st_ino;
}
int pd_upgrade_capsule_begin(struct pd_upgrade_capsule* value)
{
    if(!started_valid(value->directory,value->intent,value->intent_size)){return -1;}
    unsigned char bytes[40];started_bytes(value->intent,value->intent_size,bytes);
    int fd=openat(value->directory,"started",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    int fresh=fd>=0;
    if(fd<0 && errno==EEXIST){fd=openat(value->directory,"started",O_WRONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);}
    if(fd<0){return -1;}struct stat st;
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=(off_t)sizeof(bytes) && pd_capsule_io_file(fd,st.st_size,0600);
    size_t used=ok?(size_t)st.st_size:0;
    while(ok && used<sizeof(bytes)){
        ssize_t n=pwrite(fd,bytes+used,sizeof(bytes)-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && !fsync(value->directory) && pd_capsule_checkpoint("upgrade-started-durable")?fresh:-1;
}
static int space_ok(int parent,int directory,int worker,const unsigned char* intent,size_t intent_size,off_t size)
{
    struct statvfs space;off_t a=0,b=0;
    if(fstatvfs(parent,&space)){return 0;}uint64_t block=space.f_frsize;
    if(!block || block>UINT64_MAX/4 || space.f_bavail>UINT64_MAX/block ||
        !pd_capsule_io_existing(directory,"intent",-1,intent,(off_t)intent_size,0600,&a) ||
        !pd_capsule_io_existing(directory,"worker",worker,NULL,size,0700,&b)){return 0;}
    uint64_t needed=PD_STORE_RESERVE+(uint64_t)((off_t)intent_size-a)+(uint64_t)(size-b);
    if(needed>UINT64_MAX-4*block){return 0;}
    return pd_capsule_test_space((uint64_t)space.f_bavail*block)>=needed+4*block;
}
static int capsule_publish(int parent,int units,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected,const struct pd_upgrade_origin* origin)
{
    const char* pending=units>=0?kStaging:kPending;
    struct pd_upgrade parsed;unsigned char intent[PD_UPGRADE_CAPSULE_ORIGIN_SIZE]={0};off_t worker_size=0;
    size_t intent_size=origin?PD_UPGRADE_CAPSULE_ORIGIN_SIZE:PD_UPGRADE_CAPSULE_SIZE;
    if(!pd_upgrade_parse(approval,PD_UPGRADE_APPROVAL_SIZE,&parsed) || memcmp(key,parsed.candidate_key,32) ||
        !pd_readiness_valid(expected) || !pd_capsule_io_hash(worker,PD_UPGRADE_WORKER_MAX,intent+kWorkerHash,&worker_size)){return 0;}
    struct pd_upgrade_origin checked;
    if(origin && !pd_upgrade_origin_parse(origin->record,sizeof(origin->record),&parsed,&checked)){return 0;}
    memcpy(intent,origin?"PDUPCAP2":"PDUPCAP1",8);memcpy(intent+8,approval,PD_UPGRADE_APPROVAL_SIZE);
    if(origin){memcpy(intent+PD_UPGRADE_CAPSULE_SIZE,origin->record,sizeof(origin->record));}
    memcpy(intent+kIdentity,expected->hostname,64);memcpy(intent+kIdentity+64,expected->address,16);
    memcpy(intent+kIdentity+80,expected->dns,256);
    int guard=-1,lease=lock_parent(parent,&guard);if(lease<0){return 0;}struct stat st;
    int ok=fstatat(lease,kRetired,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    int existing=ok?openat(lease,kActive,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    if(existing>=0){
        struct pd_upgrade_capsule prior;initialize(&prior);prior.directory=existing;
        ok=validate(existing,key,&prior) && prior.intent_size==intent_size && !memcmp(intent,prior.intent,intent_size) &&
            !fsync(existing) && !fsync(lease) && (units<0 || pd_upgrade_units_arm(units));
        pd_upgrade_capsule_close(&prior);close(lease);close(guard);return ok;
    }
    ok=ok && errno==ENOENT;
    if(ok && units>=0){ok=fstatat(lease,kPending,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;}
    if(ok && mkdirat(lease,pending,0700) && errno!=EEXIST){ok=0;}
    int directory=ok?openat(lease,pending,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    ok=ok && pd_capsule_io_directory(directory,1) && inventory(directory,1) && space_ok(lease,directory,worker,intent,intent_size,worker_size) &&
        pd_capsule_io_copy(directory,"intent",-1,intent,(off_t)intent_size,0600) && pd_capsule_checkpoint("upgrade-intent-durable") &&
        pd_capsule_io_copy(directory,"worker",worker,NULL,worker_size,0700);
    if(ok){
        struct pd_upgrade_capsule staged;initialize(&staged);
        ok=validate(directory,key,&staged) && staged.intent_size==intent_size && !memcmp(intent,staged.intent,intent_size);pd_upgrade_capsule_close(&staged);
    }
    ok=ok && !fsync(directory) && !fsync(lease) && pd_capsule_checkpoint("upgrade-capsule-durable") &&
        (units<0 || (pd_service_upgrade_cleanup_idle() && pd_upgrade_units_arm(units) &&
            pd_capsule_checkpoint("upgrade-recovery-armed"))) &&
        !renameat(lease,pending,lease,kActive) && pd_capsule_checkpoint("upgrade-capsule-rename") && !fsync(lease);
    if(directory>=0){close(directory);}close(lease);close(guard);return ok;
}
int pd_upgrade_capsule_publish(int parent,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected)
{
    return capsule_publish(parent,-1,worker,key,approval,expected,NULL);
}
int pd_upgrade_capsule_publish_origin(int parent,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected,const struct pd_upgrade_origin* origin)
{
    return origin && capsule_publish(parent,-1,worker,key,approval,expected,origin);
}
int pd_upgrade_capsule_publish_armed(int parent,int units,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected,const struct pd_upgrade_origin* origin)
{
    return units>=0 && origin && capsule_publish(parent,units,worker,key,approval,expected,origin);
}
static int terminal(int store,const unsigned char key[32],struct pd_upgrade_state* state)
{
    struct stat st;
    return pd_upgrade_state_read(store,state)==1 &&
        (state->phase==PD_UPGRADE_COMMITTED || state->phase==PD_UPGRADE_ROLLED_BACK) &&
        !memcmp(state->approval.candidate_key,key,32) &&
        fstatat(store,"upgrade-transaction.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT && !fsync(store);
}
static int retired_files(int directory,const unsigned char key[32],const unsigned char* approval)
{
    if(!pd_capsule_io_directory(directory,1) || !inventory(directory,0)){return 0;}
    int fd=openat(directory,"intent",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;
    if(fd<0){return errno==ENOENT && fstatat(directory,"worker",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        fstatat(directory,"started",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;}
    unsigned char intent[PD_UPGRADE_CAPSULE_ORIGIN_SIZE];struct pd_upgrade parsed;struct pd_readiness expected;
    int ok=!fstat(fd,&st) && (st.st_size==PD_UPGRADE_CAPSULE_SIZE || st.st_size==PD_UPGRADE_CAPSULE_ORIGIN_SIZE) &&
        pd_capsule_io_file(fd,st.st_size,0600);
    size_t intent_size=ok?(size_t)st.st_size:0;
    ok=ok && pd_capsule_io_read(fd,intent,intent_size,0) && decode(intent,intent_size,key,&parsed,&expected,NULL) &&
        !memcmp(approval,parsed.record,PD_UPGRADE_APPROVAL_SIZE) && started_valid(directory,intent,intent_size);
    close(fd);if(!ok){return 0;}
    fd=openat(directory,"worker",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    unsigned char digest[32];off_t size=0;
    ok=pd_capsule_io_hash(fd,PD_UPGRADE_WORKER_MAX,digest,&size) && pd_capsule_io_file(fd,size,0700) &&
        !crypto_verify32(digest,intent+kWorkerHash);close(fd);return ok;
}
static int remove_retired(int parent,const unsigned char key[32],const unsigned char* approval)
{
    int directory=openat(parent,kRetired,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0){return errno==ENOENT && !fsync(parent);}
    int ok=retired_files(directory,key,approval);
    const char* const names[]={"started","worker","intent"};
    const char* const points[]={"upgrade-retire-started","upgrade-retire-worker","upgrade-retire-intent"};
    for(unsigned i=0;ok && i<3;++i){ok=(!unlinkat(directory,names[i],0) || errno==ENOENT) && pd_capsule_checkpoint(points[i]);}
    ok=ok && !fsync(directory);close(directory);
    return ok && !unlinkat(parent,kRetired,AT_REMOVEDIR) && !fsync(parent);
}
int pd_upgrade_capsule_retire(struct pd_upgrade_capsule* value,int store)
{
    struct pd_upgrade_state state;struct stat held,observed;
    if(!terminal(store,value->approval.candidate_key,&state) ||
        memcmp(value->approval.record,state.approval.record,PD_UPGRADE_APPROVAL_SIZE) ||
        !pd_capsule_io_directory(value->parent,0) || fstat(value->directory,&held) ||
        fstatat(value->parent,kActive,&observed,AT_SYMLINK_NOFOLLOW) || held.st_dev!=observed.st_dev || held.st_ino!=observed.st_ino ||
        !retired_files(value->directory,value->approval.candidate_key,value->approval.record) ||
        !fstatat(value->parent,kRetired,&observed,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){return 0;}
    return !renameat(value->parent,kActive,value->parent,kRetired) && pd_capsule_checkpoint("upgrade-retire-rename") &&
        !fsync(value->parent) && remove_retired(value->parent,value->approval.candidate_key,value->approval.record);
}
static int cleanup_retired(int parent,int units,int store,const unsigned char key[32])
{
    int guard=-1,lease=lock_parent(parent,&guard);if(lease<0){return 0;}
    struct pd_upgrade_state state;struct stat st;
    int ok=terminal(store,key,&state) && fstatat(lease,kActive,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        remove_retired(lease,key,state.approval.record) &&
        (units<0 || pd_upgrade_units_disarm(units,store,state.approval.record));
    close(lease);close(guard);return ok;
}
int pd_upgrade_capsule_cleanup_retired(int parent,int store,const unsigned char key[32])
{
    return cleanup_retired(parent,-1,store,key);
}
int pd_upgrade_capsule_cleanup_units(int parent,int units,int store,const unsigned char key[32])
{
    return units>=0 && cleanup_retired(parent,units,store,key);
}
