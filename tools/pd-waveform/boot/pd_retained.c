#define _GNU_SOURCE
#include "pd_retained.h"
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_capsule_io.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char* point,unsigned index);
static uid_t owner(void){return getuid();}
#else
static int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
static uid_t owner(void){return 0;}
#endif
static int directory(int parent,const char* name,int create)
{
    if(create && mkdirat(parent,name,0700) && errno!=EEXIST){return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat a,b;
    int ok=fd>=0 && !fstat(parent,&a) && !fstat(fd,&b) && S_ISDIR(b.st_mode) &&
        b.st_uid==owner() && (b.st_mode&07777)==0700 && a.st_dev==b.st_dev && (!create || !fsync(parent));
    if(!ok){if(fd>=0){close(fd);}return -1;}return fd;
}
static int metadata(int directory,int fd,off_t length,mode_t mode)
{
    struct stat a,b;
    return fd>=0 && !fstat(directory,&a) && !fstat(fd,&b) && S_ISREG(b.st_mode) && b.st_uid==owner() &&
        b.st_dev==a.st_dev && b.st_nlink==1 && b.st_size==length && (b.st_mode&07777)==mode;
}
static int terminal(const struct pd_upgrade_state* state)
{
    return state->phase==PD_UPGRADE_COMMITTED || state->phase==PD_UPGRADE_ROLLED_BACK;
}
static void name_for(const struct pd_upgrade_state* state,char name[65])
{
    static const char hex[]="0123456789abcdef";unsigned char digest[32];
    crypto_blake2b(digest,sizeof(digest),state->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    for(unsigned i=0;i<32;++i){name[i*2]=hex[digest[i]>>4];name[i*2+1]=hex[digest[i]&15];}name[64]=0;
}
static void receipt(const struct pd_upgrade_state* state,const struct pd_upgrade* pair,unsigned char bytes[PD_RETAINED_SIZE])
{
    memset(bytes,0,PD_RETAINED_SIZE);memcpy(bytes,"PDKEEP01PDUTXN01",16);
    memcpy(bytes+16,state->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    bytes[8+PD_UPGRADE_STATE_SIZE-1]=(unsigned char)state->phase;
    const struct pd_release* releases[]={&pair->previous_release,&pair->candidate_release};
    for(unsigned i=0;i<2;++i){
        unsigned char* header=bytes+8+PD_UPGRADE_STATE_SIZE+i*PD_RELEASE_PREFIX;
        memcpy(header,releases[i]->manifest,PD_RELEASE_MANIFEST);
        memcpy(header+PD_RELEASE_MANIFEST,releases[i]->signature,64);
    }
}
static int inventory(int fd,int receipts)
{
    int copy=openat(fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(listing);if(!entry){ok=!errno;break;}
        const char* name=entry->d_name;
        if(!strcmp(name,".") || !strcmp(name,"..")){continue;}
        if(receipts){
            if(!strcmp(name,"receipt") || !strcmp(name,"receipt.new") ||
                !strcmp(name,"retire") || !strcmp(name,"retire.new")){continue;}
            ok=0;break;
        }
        if(!strcmp(name,"release.header")){continue;}
        unsigned i=0;for(;i<PD_RELEASE_COUNT && strcmp(name,pd_release_names[i]);++i){}
        if(i==PD_RELEASE_COUNT){ok=0;break;}
    }
    closedir(listing);return ok;
}
static int retained_release(int stage,const char* name,const unsigned char header[PD_RELEASE_PREFIX],
                             const struct pd_release* release)
{
    int dir=directory(stage,name,0),fd=dir>=0?openat(dir,"release.header",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    unsigned char observed[PD_RELEASE_PREFIX];
    int ok=dir>=0 && inventory(dir,0) && metadata(dir,fd,sizeof(observed),0600) &&
        pd_capsule_io_read(fd,observed,sizeof(observed),0) && !memcmp(observed,header,sizeof(observed));
    if(fd>=0){close(fd);}
    for(unsigned i=0;ok && i<PD_RELEASE_COUNT;++i){
        fd=openat(dir,pd_release_names[i],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(fd<0 && errno==ENOENT && i==2){continue;}
        ok=metadata(dir,fd,release->length[i],i<2?0755:0644);
        unsigned char* bytes=ok?malloc(release->length[i]):NULL;
        ok=ok && bytes && pd_capsule_io_read(fd,bytes,release->length[i],0) &&
            pd_release_record_validate(bytes,release->length[i],i,release);
        free(bytes);if(fd>=0){close(fd);}
    }
    if(dir>=0){close(dir);}return ok;
}
int pd_retained_reopen(int store,int stage,const unsigned char key[32],uint32_t arch,
                        const struct pd_identity* identity,const struct pd_upgrade_state* prior,
                        struct pd_upgrade* upgrade)
{
    if(!terminal(prior) || memcmp(key,prior->approval.candidate_key,32)){return 0;}
    if(pd_upgrade_reopen(stage,key,arch,identity,upgrade)){
        return !memcmp(upgrade->record,prior->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    }
    char name[65];name_for(prior,name);int root=directory(store,"upgrade-retained",0);
    int record=root>=0?directory(root,name,0):-1;
    int fd=record>=0?openat(record,"receipt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    unsigned char bytes[PD_RETAINED_SIZE];struct pd_upgrade_state recorded;
    int ok=record>=0 && inventory(record,1) && metadata(record,fd,sizeof(bytes),0600) && pd_capsule_io_read(fd,bytes,sizeof(bytes),0) &&
        !memcmp(bytes,"PDKEEP01",8) && pd_upgrade_state_parse(bytes+8,PD_UPGRADE_STATE_SIZE,&recorded) &&
        recorded.phase==prior->phase && !memcmp(recorded.approval.record,prior->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    if(fd>=0){close(fd);}if(record>=0){close(record);}if(root>=0){close(root);}if(!ok){return 0;}
    unsigned choice=prior->phase==PD_UPGRADE_COMMITTED?1:0;
    ok=pd_upgrade_reopen_choice(stage,key,arch,identity,choice,upgrade) &&
        !memcmp(upgrade->record,prior->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    const unsigned char* headers[]={bytes+8+PD_UPGRADE_STATE_SIZE,bytes+8+PD_UPGRADE_STATE_SIZE+PD_RELEASE_PREFIX};
    struct pd_release releases[2];const unsigned char* keys[]={prior->approval.previous_key,prior->approval.candidate_key};
    for(unsigned i=0;ok && i<2;++i){
        ok=pd_release_header_parse(headers[i],PD_RELEASE_PREFIX,keys[i],arch,prior->approval.candidate.profile,
            prior->approval.candidate.firmware,&releases[i]);
    }
    const struct pd_release* selected=choice?&upgrade->candidate_release:&upgrade->previous_release;
    ok=ok && !memcmp(selected->manifest,headers[choice],PD_RELEASE_MANIFEST) &&
        !memcmp(selected->signature,headers[choice]+PD_RELEASE_MANIFEST,64) &&
        retained_release(stage,choice?"previous":"candidate",headers[!choice],&releases[!choice]);
    if(ok){upgrade->previous_release=releases[0];upgrade->candidate_release=releases[1];}return ok;
}
int pd_retained_compact(int store,const unsigned char key[32],uint32_t arch,const struct pd_identity* identity)
{
    struct pd_upgrade_state state;
    if(pd_upgrade_state_read(store,&state)!=1 || !terminal(&state) || memcmp(key,state.approval.candidate_key,32) ||
        !pd_upgrade_at_phase(store,state.approval.record,state.phase)){return 0;}
    struct pd_generation generation;int active=pd_generation_active(store,&generation);
    struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};struct pd_upgrade verified;
    struct stat pending;
    unsigned choice=state.phase==PD_UPGRADE_COMMITTED?1:0;
    int ok=active==1 && !generation.legacy &&
        pd_pair_acquire(generation.control,generation.stage,key,arch,identity,&pair) && pair.choice==choice &&
        fstatat(generation.control,"active.new",&pending,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        pd_retained_reopen(store,generation.stage,key,arch,identity,&state,&verified);
    unsigned char bytes[PD_RETAINED_SIZE];char name[65];name_for(&state,name);
    if(ok){receipt(&state,&verified,bytes);}
    int root=ok?directory(store,"upgrade-retained",1):-1;
    int record=root>=0?directory(root,name,1):-1;
    ok=ok && record>=0 && inventory(record,1) && pd_capsule_io_copy(record,"receipt",-1,bytes,sizeof(bytes),0600);
    int durable=ok?openat(record,"receipt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    ok=ok && metadata(record,durable,sizeof(bytes),0600) && !fsync(durable) &&
        !fsync(record) && pd_upgrade_checkpoint("retained-durable",choice);
    if(durable>=0){close(durable);}
    int obsolete=ok?directory(generation.stage,choice?"previous":"candidate",0):-1;
    int payload=obsolete>=0?openat(obsolete,"payload.bundle",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    int missing=payload<0 && errno==ENOENT;
    if(ok && !missing){
        const struct pd_release* release=choice?&verified.previous_release:&verified.candidate_release;
        struct stat pinned,named;unsigned char digest[32];off_t length=0;
        ok=metadata(obsolete,payload,release->length[2],0644) &&
            pd_capsule_io_hash(payload,release->length[2],digest,&length) &&
            !crypto_verify32(digest,release->manifest+PD_RELEASE_HEADER+2*PD_RELEASE_RECORD+4) &&
            !fstat(payload,&pinned) && !fstatat(obsolete,"payload.bundle",&named,AT_SYMLINK_NOFOLLOW) &&
            pinned.st_dev==named.st_dev && pinned.st_ino==named.st_ino &&
            pd_upgrade_checkpoint("retained-before-unlink",choice) && !unlinkat(obsolete,"payload.bundle",0) &&
            pd_upgrade_checkpoint("retained-unlinked",choice);
    }
    ok=ok && obsolete>=0 && !fsync(obsolete) && pd_upgrade_checkpoint("retained-complete",choice);
    if(payload>=0){close(payload);}if(obsolete>=0){close(obsolete);}if(record>=0){close(record);}if(root>=0){close(root);}
    pd_pair_close(&pair);pd_generation_close(&generation);return ok;
}
