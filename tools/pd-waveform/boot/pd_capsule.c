#define _GNU_SOURCE
#include "pd_capsule.h"
#include "pd_capsule_io.h"
#include "pd_store.h"
#include "pd_transaction.h"
#include "pd_worker_units.h"
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

static const char kCapsule[]="aetherpd-installation";
static const char kPending[]=".aetherpd-installation";
static const char kRetired[]=".aetherpd-installation-retired";

#ifdef PD_CAPSULE_FAULTS
extern int pd_capsule_checkpoint(const char* point);
extern uint64_t pd_capsule_test_space(uint64_t actual);
#else
static int pd_capsule_checkpoint(const char* point){(void)point;return 1;}
static uint64_t pd_capsule_test_space(uint64_t actual){return actual;}
#endif

static int capsule_lock(int parent,int* guard)
{
    *guard=-1;if(!pd_capsule_io_directory(parent,0)){errno=EINVAL;return -1;}
    int directory=openat(parent,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!pd_capsule_io_directory(directory,0)){if(directory>=0){close(directory);}errno=EINVAL;return -1;}
    int lock=openat(directory,".aetherpd-installation.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(!pd_capsule_io_file(lock,0,0600)){
        if(lock>=0){close(lock);}close(directory);errno=EINVAL;return -1;
    }
    if(flock(lock,LOCK_EX|LOCK_NB) || fsync(lock)){
        int error=errno;close(lock);close(directory);errno=error;return -1;
    }
    *guard=lock;return directory;
}
static int capsule_inventory(int directory,int partial)
{
    static const char* const names[]={"intent","worker","release","intent.new","worker.new","release.new"};
    DIR* listing=fdopendir(openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    if(!listing){return 0;}int ok=1;struct dirent* entry;errno=0;
    while((entry=readdir(listing))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        int known=0;for(unsigned i=0;i<(partial?6u:3u);++i){known|=!strcmp(entry->d_name,names[i]);}
        if(!known){ok=0;break;}
    }
    if(errno){ok=0;}closedir(listing);return ok;
}
static int capsule_input(int release,const unsigned char* key,const unsigned char* plan,struct pd_capsule* value)
{
    return pd_migration_parse(plan,PD_MIGRATION_SIZE,&value->migration) &&
        pd_approval_parse(value->migration.approval,PD_APPROVAL_SIZE,&value->approval) &&
        pd_release_read(release,key,64,1,value->approval.firmware,value->approval.digest,&value->artifact);
}
static int capsule_validate(int directory,const unsigned char* key,struct pd_capsule* value)
{
    int intent=openat(directory,"intent",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int ok=pd_capsule_io_directory(directory,1) && capsule_inventory(directory,0) &&
        pd_capsule_io_file(intent,PD_CAPSULE_SIZE,0600) && pd_capsule_io_read(intent,value->intent,PD_CAPSULE_SIZE,0) &&
        !memcmp(value->intent,"PDCAP001",8) && !memcmp(value->intent+8,key,32);
    if(intent>=0){close(intent);}if(!ok){return 0;}
    value->worker=openat(directory,"worker",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    value->release=openat(directory,"release",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;off_t size=0;unsigned char digest[32];
    return pd_capsule_io_hash(value->worker,PD_CAPSULE_WORKER_MAX,digest,&size) &&
        pd_capsule_io_file(value->worker,size,0700) && !crypto_verify32(digest,value->intent+40) &&
        !fstat(value->release,&st) && pd_capsule_io_file(value->release,st.st_size,0600) &&
        capsule_input(value->release,key,value->intent+72,value);
}
void pd_capsule_close(struct pd_capsule* value)
{
    if(value->worker>=0){close(value->worker);}if(value->release>=0){close(value->release);}
    if(value->directory>=0){close(value->directory);}if(value->parent>=0){close(value->parent);}if(value->lock>=0){close(value->lock);}
    value->worker=value->release=value->directory=value->parent=value->lock=-1;
}
static void capsule_init(struct pd_capsule* value)
{
    memset(value,0,sizeof(*value));value->worker=value->release=value->directory=value->parent=value->lock=-1;
}
int pd_capsule_open(int parent,const unsigned char key[32],struct pd_capsule* value)
{
    capsule_init(value);value->parent=capsule_lock(parent,&value->lock);if(value->parent<0){return 0;}
    value->directory=openat(value->parent,kCapsule,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!capsule_validate(value->directory,key,value) || fsync(value->directory) || fsync(value->parent)){
        pd_capsule_close(value);errno=EINVAL;return 0;
    }
    return 1;
}
int pd_capsule_self(const struct pd_capsule* capsule,int executable)
{
    struct stat running,stored;
    return executable>=0 && capsule->worker>=0 && !fstat(executable,&running) && !fstat(capsule->worker,&stored) &&
        S_ISREG(running.st_mode) && running.st_dev==stored.st_dev && running.st_ino==stored.st_ino;
}

static int capsule_space(int parent,int directory,int worker,int release,const unsigned char* intent,off_t worker_size,off_t release_size)
{
    struct statvfs space;if(fstatvfs(parent,&space) || !space.f_frsize || space.f_bavail>UINT64_MAX/space.f_frsize){return 0;}
    off_t a=0,b=0,c=0;
    if(!pd_capsule_io_existing(directory,"intent",-1,intent,PD_CAPSULE_SIZE,0600,&a) ||
        !pd_capsule_io_existing(directory,"worker",worker,NULL,worker_size,0700,&b) ||
        !pd_capsule_io_existing(directory,"release",release,NULL,release_size,0600,&c)){return 0;}
    uint64_t needed=PD_STORE_RESERVE+(uint64_t)(PD_CAPSULE_SIZE-a)+(uint64_t)(worker_size-b)+(uint64_t)(release_size-c);
    return pd_capsule_test_space((uint64_t)space.f_bavail*space.f_frsize)>=needed;
}
int pd_capsule_publish(int parent,int worker,int release,const unsigned char key[32],const unsigned char plan[PD_MIGRATION_SIZE])
{
    struct pd_capsule input;capsule_init(&input);off_t worker_size=0;unsigned char expected[PD_CAPSULE_SIZE]={0};struct stat st;
    memcpy(expected,"PDCAP001",8);memcpy(expected+8,key,32);memcpy(expected+72,plan,PD_MIGRATION_SIZE);
    if(!capsule_input(release,key,plan,&input) || !pd_capsule_io_hash(worker,PD_CAPSULE_WORKER_MAX,expected+40,&worker_size) ||
        fstat(release,&st)){return 0;}off_t release_size=st.st_size;
    int guard=-1,lease=capsule_lock(parent,&guard);if(lease<0){return 0;}
    if(!fstatat(lease,kRetired,&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){close(lease);close(guard);return 0;}
    int existing=openat(lease,kCapsule,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(existing>=0){
        struct pd_capsule prior;capsule_init(&prior);prior.directory=existing;
        int ok=capsule_validate(existing,key,&prior) && !memcmp(expected,prior.intent,sizeof(expected)) && !fsync(existing) && !fsync(lease);
        pd_capsule_close(&prior);close(lease);close(guard);return ok;
    }
    if(errno!=ENOENT){close(lease);close(guard);return 0;}
    if(mkdirat(lease,kPending,0700) && errno!=EEXIST){close(lease);close(guard);return 0;}
    int directory=openat(lease,kPending,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int ok=pd_capsule_io_directory(directory,1) && capsule_inventory(directory,1) &&
        capsule_space(lease,directory,worker,release,expected,worker_size,release_size) &&
        pd_capsule_io_copy(directory,"intent",-1,expected,sizeof(expected),0600) && pd_capsule_checkpoint("intent-durable") &&
        pd_capsule_io_copy(directory,"worker",worker,NULL,worker_size,0700) &&
        pd_capsule_io_copy(directory,"release",release,NULL,release_size,0600);
    if(ok){
        struct pd_capsule staged;capsule_init(&staged);
        ok=capsule_validate(directory,key,&staged) && !memcmp(staged.intent,expected,sizeof(expected));pd_capsule_close(&staged);
    }
    ok=ok && !fsync(directory) && pd_capsule_checkpoint("capsule-durable") &&
        !renameat(lease,kPending,lease,kCapsule) && pd_capsule_checkpoint("capsule-rename") && !fsync(lease);
    if(directory>=0){close(directory);}close(lease);close(guard);return ok;
}
static int capsule_terminal(int store,const unsigned char* plan)
{
    return (pd_transaction_at_phase(store,plan,PD_TRANSACTION_COMMITTED) ||
        pd_transaction_at_phase(store,plan,PD_TRANSACTION_ROLLED_BACK)) && !fsync(store);
}
static int capsule_retired_files(int directory,const unsigned char* key,const unsigned char* plan)
{
    if(!pd_capsule_io_directory(directory,1) || !capsule_inventory(directory,0)){return 0;}
    int intent=openat(directory,"intent",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char bytes[PD_CAPSULE_SIZE];struct stat st;
    if(intent<0){
        return errno==ENOENT && fstatat(directory,"worker",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
            fstatat(directory,"release",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    }
    int ok=pd_capsule_io_file(intent,sizeof(bytes),0600) && pd_capsule_io_read(intent,bytes,sizeof(bytes),0) &&
        !memcmp(bytes,"PDCAP001",8) && !memcmp(bytes+8,key,32) && !memcmp(bytes+72,plan,PD_MIGRATION_SIZE);
    close(intent);if(!ok){return 0;}
    int worker=openat(directory,"worker",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(worker>=0){
        unsigned char digest[32];off_t size=0;
        ok=pd_capsule_io_hash(worker,PD_CAPSULE_WORKER_MAX,digest,&size) && pd_capsule_io_file(worker,size,0700) &&
            !crypto_verify32(digest,bytes+40);close(worker);
    }else{ok=errno==ENOENT;}
    if(!ok){return 0;}
    int release=openat(directory,"release",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(release>=0){
        struct pd_capsule value;capsule_init(&value);
        ok=!fstat(release,&st) && pd_capsule_io_file(release,st.st_size,0600) && capsule_input(release,key,plan,&value);close(release);
    }else{ok=errno==ENOENT;}return ok;
}
static int capsule_remove_retired(int parent,const unsigned char* key,const unsigned char* plan)
{
    int directory=openat(parent,kRetired,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0){return errno==ENOENT && !fsync(parent);}
    int ok=capsule_retired_files(directory,key,plan);
    const char* const names[]={"release","worker","intent"};
    for(unsigned i=0;ok && i<3;++i){
        ok=!unlinkat(directory,names[i],0) || errno==ENOENT;
        char point[48];snprintf(point,sizeof(point),"retire-%s",names[i]);ok=ok && pd_capsule_checkpoint(point);
    }
    ok=ok && !fsync(directory);close(directory);
    return ok && !unlinkat(parent,kRetired,AT_REMOVEDIR) && !fsync(parent);
}
int pd_capsule_retire(struct pd_capsule* capsule,int store)
{
    struct stat held,observed;
    if(!capsule_terminal(store,capsule->intent+72) || !pd_capsule_io_directory(capsule->parent,0) ||
        fstat(capsule->directory,&held) || fstatat(capsule->parent,kCapsule,&observed,AT_SYMLINK_NOFOLLOW) ||
        held.st_dev!=observed.st_dev || held.st_ino!=observed.st_ino ||
        !capsule_retired_files(capsule->directory,capsule->intent+8,capsule->intent+72) ||
        !fstatat(capsule->parent,kRetired,&observed,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){return 0;}
    return !renameat(capsule->parent,kCapsule,capsule->parent,kRetired) &&
        pd_capsule_checkpoint("retire-rename") && !fsync(capsule->parent) &&
        capsule_remove_retired(capsule->parent,capsule->intent+8,capsule->intent+72);
}
int pd_capsule_cleanup_retired(int parent,int store,const unsigned char key[32],const unsigned char plan[PD_MIGRATION_SIZE])
{
    if(!capsule_terminal(store,plan)){return 0;}int guard=-1,lease=capsule_lock(parent,&guard);if(lease<0){return 0;}
    int ok=capsule_remove_retired(lease,key,plan);close(lease);close(guard);return ok;
}
int pd_capsule_finish_cleanup(int parent,int units,int store,const unsigned char key[32],const unsigned char plan[PD_MIGRATION_SIZE])
{
    if(!capsule_terminal(store,plan)){return 0;}int guard=-1,lease=capsule_lock(parent,&guard);if(lease<0){return 0;}
    struct stat st;int ok=fstatat(lease,kCapsule,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        capsule_remove_retired(lease,key,plan) && pd_worker_units_disarm(units,store,plan);
    close(lease);close(guard);return ok;
}

int pd_capsule_reconcile_terminal(int parent,int units,int store,const unsigned char key[32])
{
    int fd=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    unsigned char plan[PD_MIGRATION_SIZE];struct pd_migration parsed;
    int ok=pd_capsule_io_file(fd,sizeof(plan),0600) && pd_capsule_io_read(fd,plan,sizeof(plan),0) &&
        pd_migration_parse(plan,sizeof(plan),&parsed);close(fd);
    if(!ok){return 0;}
    if(!pd_transaction_at_phase(store,plan,PD_TRANSACTION_COMMITTED) &&
        !pd_transaction_at_phase(store,plan,PD_TRANSACTION_ROLLED_BACK)){return 1;}
    return pd_capsule_finish_cleanup(parent,units,store,key,plan);
}
