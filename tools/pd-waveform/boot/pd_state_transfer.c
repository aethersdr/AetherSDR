#define _GNU_SOURCE
#include "pd_state_transfer.h"
#include "pd_store.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { kStateMax=65536,kRecordSize=8+PD_APPROVAL_SIZE+72 };
struct state_snapshot {unsigned char bytes[2][kStateMax],record[kRecordSize];size_t size[2];};
struct state_ownership {uid_t source,target;gid_t group;const char* magic;};
#ifdef PD_STATE_TEST
static uid_t state_root(void){return getuid();}
static uid_t state_user(void){return getuid();}
static gid_t state_group(void){return getgid();}
static gid_t state_root_group(void){return getgid();}
static int state_quiet(void){return 1;}
#else
static uid_t state_root(void){return 0;}
static uid_t state_user(void){return 78666;}
static gid_t state_group(void){return 20;}
static gid_t state_root_group(void){return 0;}
static int state_quiet(void){struct stat st;return lstat("/run/aetherpd",&st)<0 && errno==ENOENT;}
#endif
#ifdef PD_STATE_FAULTS
extern int pd_state_checkpoint(const char* point);
#else
static int pd_state_checkpoint(const char* point){(void)point;return 1;}
#endif
static int state_meta(int fd,uid_t owner,mode_t mode,int directory,struct stat* st)
{
    return fd>=0 && !fstat(fd,st) && (directory?S_ISDIR(st->st_mode):S_ISREG(st->st_mode)) &&
        st->st_uid==owner && (st->st_mode&07777)==mode && (directory || st->st_nlink==1);
}
static int state_dir(int parent,const char* name)
{
    return openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static int state_file(int parent,const char* name)
{
    return openat(parent,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
}
static int state_read(int fd,unsigned char* bytes,size_t size)
{
    size_t offset=0;
    while(offset<size){
        ssize_t n=pread(fd,bytes+offset,size-offset,(off_t)offset);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}offset+=(size_t)n;
    }
    return 1;
}
static int state_write(int fd,const unsigned char* bytes,size_t size)
{
    while(size){
        ssize_t n=write(fd,bytes,size);if(n<0 && errno==EINTR){continue;}
        if(n<=0){return 0;}bytes+=n;size-=(size_t)n;
    }
    return 1;
}
static int state_bytes_equal(int fd,const unsigned char* bytes,size_t size)
{
    unsigned char actual[1024];size_t offset=0;int ok=1;
    while(ok && offset<size){
        size_t count=size-offset;if(count>sizeof(actual)){count=sizeof(actual);}
        ssize_t n=pread(fd,actual,count,(off_t)offset);
        if(n<0 && errno==EINTR){continue;}
        if(n<=0 || memcmp(actual,bytes+offset,(size_t)n)){ok=0;break;}offset+=(size_t)n;
    }
    crypto_wipe(actual,sizeof(actual));return ok;
}
static int state_entries(int directory,const char* first,const char* second,const char* third)
{
    /* A fresh description avoids sharing the directory cursor with a retry. */
    int scan=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scan<0){return 0;}
    DIR* listing=fdopendir(scan);if(!listing){close(scan);return 0;}
    int ok=1;struct dirent* entry;errno=0;
    while((entry=readdir(listing))){
        const char* name=entry->d_name;
        if(!strcmp(name,".") || !strcmp(name,"..")){continue;}
        if((first && !strcmp(name,first)) || (second && !strcmp(name,second)) ||
            (third && !strcmp(name,third))){continue;}
        ok=0;break;
    }
    if(errno){ok=0;}closedir(listing);return ok;
}
static int state_empty_target(int store,const struct state_ownership* owner)
{
    int fd=state_dir(store,"state");struct stat st;
    int ok=state_meta(fd,owner->target,0700,1,&st) && st.st_gid==owner->group && state_entries(fd,NULL,NULL,NULL);
    if(fd>=0){close(fd);}return ok;
}
static int state_copy(int parent,const char* name,const unsigned char* bytes,size_t size)
{
    struct stat st;int fd=state_file(parent,name);
    if(fd>=0){
        int ok=state_meta(fd,state_root(),0600,0,&st) && st.st_size==(off_t)size && state_bytes_equal(fd,bytes,size);
        close(fd);return ok;
    }
    if(errno!=ENOENT){return 0;}
    char temporary[64];int count=snprintf(temporary,sizeof(temporary),"%s.new",name);
    if(count<=0 || count>=(int)sizeof(temporary)){return 0;}
    fd=state_file(parent,temporary);
    if(fd>=0){
        int ok=state_meta(fd,state_root(),0600,0,&st) && st.st_size>=0 && st.st_size<=(off_t)size &&
            state_bytes_equal(fd,bytes,(size_t)st.st_size);
        close(fd);if(!ok || unlinkat(parent,temporary,0)){return 0;}
    }else if(errno!=ENOENT){return 0;}
    fd=openat(parent,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}int ok=state_write(fd,bytes,size) && !fsync(fd);if(close(fd)){ok=0;}
    return ok && !renameat(parent,temporary,parent,name) && !fsync(parent);
}
static void state_length(unsigned char* p,size_t size)
{
    p[0]=(unsigned char)(size>>24);p[1]=(unsigned char)(size>>16);p[2]=(unsigned char)(size>>8);p[3]=(unsigned char)size;
}
static size_t state_size(const unsigned char* p)
{
    return ((size_t)p[0]<<24)|((size_t)p[1]<<16)|((size_t)p[2]<<8)|p[3];
}
static int state_record(int parent,const char* name,unsigned char record[kRecordSize],const unsigned char* approval,const char* magic)
{
    int fd=state_file(parent,name);if(fd<0){return errno==ENOENT?0:-1;}
    struct stat st;int ok=state_meta(fd,state_root(),0600,0,&st) && st.st_size==kRecordSize &&
        state_read(fd,record,kRecordSize);close(fd);
    if(!ok || memcmp(record,magic,8) || memcmp(record+8,approval,PD_APPROVAL_SIZE)){return -1;}
    for(unsigned i=0;i<2;++i){size_t size=state_size(record+8+PD_APPROVAL_SIZE+36*i);if(!size || size>kStateMax){return -1;}}
    return 1;
}
static int state_capture(struct state_snapshot* snapshot,int inputs[2],const struct state_ownership* policy,const unsigned char* approval)
{
    memcpy(snapshot->record,policy->magic,8);memcpy(snapshot->record+8,approval,PD_APPROVAL_SIZE);
    uid_t owner=policy->source;
    for(unsigned i=0;i<2;++i){
        struct stat before,after;
        if(!state_meta(inputs[i],owner,0600,0,&before) || before.st_size<=0 || before.st_size>kStateMax){return 0;}
        snapshot->size[i]=(size_t)before.st_size;
        if(!state_read(inputs[i],snapshot->bytes[i],snapshot->size[i]) ||
            !state_bytes_equal(inputs[i],snapshot->bytes[i],snapshot->size[i]) ||
            !state_meta(inputs[i],owner,0600,0,&after) || before.st_size!=after.st_size ||
            before.st_mtime!=after.st_mtime || before.st_ctime!=after.st_ctime){return 0;}
        unsigned char* entry=snapshot->record+8+PD_APPROVAL_SIZE+36*i;
        state_length(entry,snapshot->size[i]);crypto_blake2b(entry+4,32,snapshot->bytes[i],snapshot->size[i]);
    }
    if(!pd_state_checkpoint("sources-captured")){return 0;}
    for(unsigned i=0;i<2;++i){
        struct stat st;
        if(!state_meta(inputs[i],owner,0600,0,&st) || st.st_size!=(off_t)snapshot->size[i] ||
            !state_bytes_equal(inputs[i],snapshot->bytes[i],snapshot->size[i])){return 0;}
    }
    return 1;
}
static int state_tree(int directory,const unsigned char* record,int released,const struct state_ownership* owner)
{
    struct stat st;int ok=state_meta(directory,released?owner->target:state_root(),0700,1,&st);
    if(released){ok=ok && st.st_gid==owner->group;}
    else{ok=ok && state_entries(directory,"provision.json","tsnet",NULL);}
    int ts=ok?state_dir(directory,"tsnet"):-1;
    ok=ok && ts>=0 && !fstat(ts,&st) && S_ISDIR(st.st_mode) && (st.st_mode&07777)==0700 &&
        (st.st_uid==owner->target || (!released && st.st_uid==state_root())) && (!released || st.st_gid==owner->group);
    if(!released){ok=ok && state_entries(ts,"tailscaled.state",NULL,NULL);}
    const char* const names[]={"provision.json","tailscaled.state"};int parents[]={directory,ts};
    unsigned char* bytes=malloc(kStateMax);if(!bytes){ok=0;}
    for(unsigned i=0;ok && i<2;++i){
        int fd=state_file(parents[i],names[i]);
        ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_nlink==1 && (st.st_mode&07777)==0600 &&
            st.st_size>0 && st.st_size<=kStateMax && (st.st_uid==owner->target || (!released && st.st_uid==state_root()));
        if(released){ok=ok && st.st_gid==owner->group;}
        if(ok && !released){
            const unsigned char* entry=record+8+PD_APPROVAL_SIZE+36*i;unsigned char digest[32];
            ok=st.st_size==(off_t)state_size(entry) && state_read(fd,bytes,(size_t)st.st_size);
            if(ok){crypto_blake2b(digest,32,bytes,(size_t)st.st_size);ok=!crypto_verify32(digest,entry+4);}
        }
        if(fd>=0){close(fd);}
    }
    if(bytes){crypto_wipe(bytes,kStateMax);free(bytes);}if(ts>=0){close(ts);}return ok;
}
static int state_release(int directory,const struct state_ownership* owner)
{
    int ts=state_dir(directory,"tsnet");if(ts<0){return 0;}
    const char* const names[]={"provision.json","tailscaled.state"};int parents[]={directory,ts};int ok=1;
    for(unsigned i=0;ok && i<2;++i){
        int fd=state_file(parents[i],names[i]);
        ok=fd>=0 && !fchown(fd,owner->target,owner->group) && !fsync(fd);
        if(fd>=0){close(fd);}ok=ok && pd_state_checkpoint(i?"identity-owned":"config-owned");
    }
    ok=ok && !fchown(ts,owner->target,owner->group) && !fsync(ts);close(ts);
    ok=ok && pd_state_checkpoint("nested-owned") && !fchown(directory,owner->target,owner->group) && !fsync(directory);
    return ok && pd_state_checkpoint("state-released");
}
static int state_finish(int store,int directory,const unsigned char* record,const struct state_ownership* owner)
{
    int ok=state_tree(directory,record,0,owner) && state_copy(store,"identity-transferred",record,kRecordSize) &&
        pd_state_checkpoint("completion-recorded") && state_release(directory,owner) && !fsync(store);
    if(ok && unlinkat(store,"identity-transfer",0) && errno!=ENOENT){ok=0;}
    return ok && !fsync(store);
}
static int state_prepare(int store,struct state_snapshot* snapshot)
{
    int made=!mkdirat(store,".identity-transfer",0700);
    if(!made && errno!=EEXIST){return -1;}
    int stage=state_dir(store,".identity-transfer");struct stat st;
    int ok=state_meta(stage,state_root(),0700,1,&st) && pd_state_checkpoint("stage-created");
    unsigned char prior[kRecordSize];int intent=ok?state_record(stage,"intent",prior,snapshot->record+8,(const char*)snapshot->record):-1;
    if(intent==1){ok=!memcmp(prior,snapshot->record,kRecordSize);}
    else if(intent==0){ok=state_entries(stage,"intent.new",NULL,NULL);}
    else{ok=0;}
    ok=ok && state_copy(stage,"intent",snapshot->record,kRecordSize) && pd_state_checkpoint("intent-recorded");
    int ts=-1;
    if(ok){
        int nested=!mkdirat(stage,"tsnet",0700);
        ok=nested || errno==EEXIST;
        if(ok){ts=state_dir(stage,"tsnet");ok=state_meta(ts,state_root(),0700,1,&st);}
    }
    ok=ok && state_copy(stage,"provision.json",snapshot->bytes[0],snapshot->size[0]) && pd_state_checkpoint("config-copied") &&
        state_copy(ts,"tailscaled.state",snapshot->bytes[1],snapshot->size[1]) && pd_state_checkpoint("identity-copied");
    if(ts>=0){close(ts);}
    if(ok){
        ok=state_entries(stage,"intent","provision.json","tsnet") &&
            state_copy(store,"identity-transfer",snapshot->record,kRecordSize) && pd_state_checkpoint("snapshot-prepared");
    }
    if(!ok){if(stage>=0){close(stage);}return -1;}return stage;
}
static int state_locked(int store,int provision,int tailscale,const unsigned char* approval,const struct state_ownership* owner)
{
    unsigned char record[kRecordSize];int done=state_record(store,"identity-transferred",record,approval,owner->magic);
    if(done<0){return 0;}
    if(done==1){
        int current=state_dir(store,"state");struct stat st;
        int released=state_meta(current,owner->target,0700,1,&st) && st.st_gid==owner->group;
        int ok=released?state_tree(current,record,1,owner):state_finish(store,current,record,owner);
        if(current>=0){close(current);}
        if(ok){
            unsigned char pending[kRecordSize];int present=state_record(store,"identity-transfer",pending,approval,owner->magic);
            ok=present==0 || (present==1 && !memcmp(pending,record,kRecordSize) && !unlinkat(store,"identity-transfer",0) && !fsync(store));
        }
        return ok && !fsync(store);
    }
    int prepared=state_record(store,"identity-transfer",record,approval,owner->magic);
    if(prepared<0){return 0;}
    int stage=-1;
    if(prepared==0){
        if(!state_empty_target(store,owner)){return 0;}
        struct state_snapshot* snapshot=calloc(1,sizeof(*snapshot));if(!snapshot){return 0;}
        int inputs[]={provision,tailscale};
        if(state_capture(snapshot,inputs,owner,approval)){
            stage=state_prepare(store,snapshot);memcpy(record,snapshot->record,kRecordSize);
        }
        crypto_wipe(snapshot,sizeof(*snapshot));free(snapshot);
        if(stage<0){return 0;}
    }else{
        stage=state_dir(store,".identity-transfer");
        if(stage<0){
            if(errno!=ENOENT){return 0;}
            int current=state_dir(store,"state");int ok=state_finish(store,current,record,owner);
            if(current>=0){close(current);}return ok;
        }
    }
    struct stat st;int ok=state_meta(stage,state_root(),0700,1,&st) && state_empty_target(store,owner);
    if(ok){
        unsigned char intent[kRecordSize];int exists=state_record(stage,"intent",intent,approval,owner->magic);
        ok=exists==0 || (exists==1 && !memcmp(intent,record,kRecordSize) && !unlinkat(stage,"intent",0));
    }
    ok=ok && state_tree(stage,record,0,owner) && !fsync(stage) &&
        !renameat(store,".identity-transfer",store,"state") && !fsync(store) && pd_state_checkpoint("state-published");
    if(ok){ok=state_finish(store,stage,record,owner);}close(stage);return ok;
}
static int state_transfer(int store,int provision,int tailscale,const unsigned char* approval,int restore)
{
    struct stat st;struct pd_approval context;
    if(!pd_approval_parse(approval,PD_APPROVAL_SIZE,&context) || (restore && context.profile!=1) ||
        fstat(store,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=state_root() || (st.st_mode&022)){return 0;}
    const struct state_ownership owner={restore?state_user():(context.profile==1?state_root():state_user()),
        restore?state_root():state_user(),restore?state_root_group():state_group(),restore?"PDROLL01":"PDSTATE1"};
    int lock=pd_store_lock(store);if(lock<0){return 0;}
    int ok=state_quiet() && state_locked(store,provision,tailscale,approval,&owner);close(lock);return ok;
}
int pd_state_transfer(int store,int provision,int tailscale,const unsigned char approval[PD_APPROVAL_SIZE])
{
    return state_transfer(store,provision,tailscale,approval,0);
}
int pd_state_restore_legacy(int store,int provision,int tailscale,const unsigned char approval[PD_APPROVAL_SIZE])
{
    return state_transfer(store,provision,tailscale,approval,1);
}
