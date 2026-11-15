#define _GNU_SOURCE
#include "pd_retire.h"
#include "pd_retained.h"
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_capsule_io.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char*,unsigned);
static uid_t owner(void){return getuid();}
#else
static int pd_upgrade_checkpoint(const char* p,unsigned i){(void)p;(void)i;return 1;}
static uid_t owner(void){return 0;}
#endif
/* All paths are fixed below two approval-derived private directories. */
enum { kEntries=23,kHeader=16+PD_UPGRADE_ORIGIN_SIZE,kEntry=40,kSize=PD_RETIRE_RECORD_SIZE };
struct tree {int parent,generation,stage,control,release[2],receipts,record;};
static void close_tree(struct tree* t)
{
    int fds[]={t->record,t->receipts,t->release[0],t->release[1],t->control,t->stage,t->generation,t->parent};
    for(unsigned i=0;i<sizeof(fds)/sizeof(fds[0]);++i){if(fds[i]>=0){close(fds[i]);}}
}
static void name_for(const unsigned char* approval,char name[65])
{
    unsigned char hash[32];static const char hex[]="0123456789abcdef";
    crypto_blake2b(hash,32,approval,PD_UPGRADE_APPROVAL_SIZE);
    for(unsigned i=0;i<32;++i){name[2*i]=hex[hash[i]>>4];name[2*i+1]=hex[hash[i]&15];}name[64]=0;
}
/* -1 absent, -2 unsafe. */
static int directory(int parent,const char* name)
{
    if(parent<0){return parent;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat a,b;
    if(fd<0){return errno==ENOENT?-1:-2;}
    if(fstat(parent,&a) || fstat(fd,&b) || a.st_dev!=b.st_dev || b.st_uid!=owner() ||
        !S_ISDIR(b.st_mode) || (b.st_mode&07777)!=0700){close(fd);return -2;}return fd;
}
static int inventory(int fd,const char* const* names,unsigned count)
{
    if(fd<0){return fd==-1;}int copy=openat(fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* item=readdir(listing);if(!item){ok=!errno;break;}
        if(!strcmp(item->d_name,".") || !strcmp(item->d_name,"..")){continue;}
        unsigned i=0;for(;i<count && strcmp(item->d_name,names[i]);++i){}
        if(i==count){ok=0;break;}
    }
    closedir(listing);return ok;
}
static int open_tree(int store,const char* name,int legacy,struct tree* t)
{
    *t=(struct tree){-1,-1,-1,-1,{-1,-1},-1,-1};
    t->parent=legacy?fcntl(store,F_DUPFD_CLOEXEC,6):directory(store,"upgrade-generations");
    t->generation=legacy?fcntl(store,F_DUPFD_CLOEXEC,6):directory(t->parent,name);
    t->stage=directory(t->generation,legacy?"upgrade-pair":"pair");
    t->control=directory(t->generation,legacy?"upgrade-control":"control");
    t->release[0]=directory(t->stage,"previous");t->release[1]=directory(t->stage,"candidate");
    t->receipts=directory(store,"upgrade-retained");t->record=directory(t->receipts,name);
    const char* const gen[]={"pair","control"},* const stage[]={"approved","previous","candidate"};
    const char* const control[]={"active","lock"},* const records[]={"receipt","retire"};
    const char* leaves[PD_RELEASE_COUNT+1];leaves[0]="release.header";
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){leaves[i+1]=pd_release_names[i];}
    return t->parent!=-2 && t->generation!=-2 && (legacy || inventory(t->generation,gen,2)) &&
        inventory(t->stage,stage,3) && inventory(t->control,control,2) &&
        inventory(t->release[0],leaves,PD_RELEASE_COUNT+1) && inventory(t->release[1],leaves,PD_RELEASE_COUNT+1) &&
        t->receipts!=-2 && inventory(t->record,records,2);
}
static void member(const struct tree* t,unsigned i,int* dir,const char** name,mode_t* mode)
{
    *mode=0600;
    if(i<18){unsigned slot=i/9,index=i%9;*dir=t->release[slot];*name=index?pd_release_names[index-1]:"release.header";
        *mode=index?(index<=2?0755:0644):0600;
    }else if(i==18){*dir=t->stage;*name="approved";}
    else if(i<21){*dir=t->control;*name=i==19?"active":"lock";}
    else{*dir=t->record;*name=i==21?"receipt":"retire";}
}
static uint32_t number(const unsigned char* p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void encode(unsigned char* p,uint32_t value)
{p[0]=value>>24;p[1]=value>>16;p[2]=value>>8;p[3]=value;}
static int fingerprint(int directory,const char* name,mode_t mode,unsigned char entry[kEntry],int capture)
{
    if(directory==-1){return capture?(memset(entry,0,kEntry),1):1;}
    if(directory<0){return 0;}
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?(capture?(memset(entry,0,kEntry),1):1):0;}
    struct stat st,parent;unsigned char hash[32];off_t length=0;
    int ok=!fstat(fd,&st) && !fstat(directory,&parent) && S_ISREG(st.st_mode) && st.st_uid==owner() &&
        st.st_dev==parent.st_dev && st.st_nlink==1 && (st.st_mode&07777)==mode &&
        st.st_size>=0 && st.st_size<=PD_RELEASE_MAX;
    if(ok && st.st_size==0){crypto_blake2b(hash,32,NULL,0);}
    else if(ok){ok=pd_capsule_io_hash(fd,PD_RELEASE_MAX,hash,&length);}
    if(ok && capture){memset(entry,0,kEntry);entry[0]=1;encode(entry+4,(uint32_t)length);memcpy(entry+8,hash,32);}
    else if(ok){ok=entry[0]==1 && number(entry+4)==length && !memcmp(hash,entry+8,32);}
    close(fd);return ok;
}
static int manifest_valid(const unsigned char data[kSize],const struct pd_upgrade* next)
{
    struct pd_upgrade_origin parsed;unsigned char digest[32];
    crypto_blake2b(digest,32,data,kSize-32);
    if(memcmp(data,"PDRET001",8) || data[8]>1 || memcmp(data+9,(unsigned char[7]){0},7) ||
        crypto_verify32(digest,data+kSize-32) ||
        !pd_upgrade_origin_parse(data+16,PD_UPGRADE_ORIGIN_SIZE,next,&parsed) || !parsed.has_predecessor){return 0;}
    for(unsigned i=0;i<kEntries;++i){
        const unsigned char* e=data+kHeader+i*kEntry;
        if(e[0]>1 || e[1] || e[2] || e[3] || number(e+4)>PD_RELEASE_MAX ||
            (!e[0] && memcmp(e,(unsigned char[kEntry]){0},kEntry))){return 0;}
    }
    return 1;
}
static int receipt_matches(int dir,const struct pd_upgrade_state* state,const struct pd_upgrade* pair)
{
    if(dir==-1){return 1;}int fd=openat(dir,"receipt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    unsigned char expected[PD_RETAINED_SIZE]={0};memcpy(expected,"PDKEEP01PDUTXN01",16);
    memcpy(expected+16,state->approval.record,PD_UPGRADE_APPROVAL_SIZE);expected[8+PD_UPGRADE_STATE_SIZE-1]=state->phase;
    const struct pd_release* releases[]={&pair->previous_release,&pair->candidate_release};
    for(unsigned i=0;i<2;++i){unsigned char* header=expected+8+PD_UPGRADE_STATE_SIZE+i*PD_RELEASE_PREFIX;
        memcpy(header,releases[i]->manifest,PD_RELEASE_MANIFEST);memcpy(header+PD_RELEASE_MANIFEST,releases[i]->signature,64);}
    int ok=pd_capsule_io_file(fd,sizeof(expected),0600) && pd_capsule_io_equal(fd,-1,expected,sizeof(expected));
    close(fd);return ok;
}
static int read_manifest(int record,unsigned char bytes[kSize])
{
    int fd=openat(record,"retire",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    struct stat a,b;
    int ok=!fstat(fd,&a) && !fstat(record,&b) && a.st_dev==b.st_dev &&
        pd_capsule_io_file(fd,kSize,0600) && pd_capsule_io_read(fd,bytes,kSize,0);
    close(fd);return ok?1:-1;
}
static int absent(int dir,const char* name)
{
    struct stat st;return dir==-1 || (dir>=0 && fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT);
}
static int remove_directory(int parent,const char* name,unsigned point)
{
    if(parent==-1){return 1;}
    if(unlinkat(parent,name,AT_REMOVEDIR) && errno!=ENOENT){return 0;}
    return !fsync(parent) && pd_upgrade_checkpoint("retire-directory",point);
}
int pd_retire_ready(int store,const struct pd_upgrade_state* predecessor)
{
    char name[65];name_for(predecessor->approval.record,name);
    int receipts=directory(store,"upgrade-retained"),record=directory(receipts,name);
    unsigned char bytes[kSize];int present=record==-1?0:record>=0?read_manifest(record,bytes):-1;
    int ok=present>=0 && absent(record,"retire.new");
    if(ok && present){
        struct pd_upgrade_origin origin;struct tree old={-1,-1,-1,-1,{-1,-1},-1,-1};
        ok=manifest_valid(bytes,&predecessor->approval) &&
            pd_upgrade_origin_parse(bytes+16,PD_UPGRADE_ORIGIN_SIZE,&predecessor->approval,&origin);
        if(ok){name_for(origin.predecessor.approval.record,name);
            ok=open_tree(store,name,bytes[8],&old) && old.stage==-1 && old.control==-1 &&
                (bytes[8] || old.generation==-1) && old.record==-1;}
        close_tree(&old);
    }
    if(record>=0){close(record);}if(receipts>=0){close(receipts);}return ok;
}
int pd_retire_predecessor(int store,const unsigned char key[32],uint32_t arch,
                           const struct pd_identity* identity,const struct pd_upgrade_origin* supplied)
{
    struct pd_upgrade_state state;struct pd_upgrade_origin origin;
    if(pd_upgrade_state_read(store,&state)!=1 ||
        (state.phase!=PD_UPGRADE_COMMITTED && state.phase!=PD_UPGRADE_ROLLED_BACK) ||
        memcmp(key,state.approval.candidate_key,32) || !pd_upgrade_at_phase(store,state.approval.record,state.phase) ||
        !pd_upgrade_origin_parse(supplied->record,sizeof(supplied->record),&state.approval,&origin)){return 0;}
    if(!origin.has_predecessor){return 1;}
    struct pd_generation current;struct pd_pair selected={.lease=-1,.boot=-1,.payload=-1};
    int ok=pd_generation_active(store,&current)==1 && !current.legacy &&
        pd_pair_acquire(current.control,current.stage,key,arch,identity,&selected) &&
        selected.choice==(state.phase==PD_UPGRADE_COMMITTED?1:0) && absent(current.control,"active.new");
    char oldname[65],newname[65];name_for(origin.predecessor.approval.record,oldname);name_for(state.approval.record,newname);
    ok=ok && strcmp(oldname,newname);
    int receipts=ok?directory(store,"upgrade-retained"):-1,record=directory(receipts,newname);
    unsigned char bytes[kSize]={0};int present=record>=0?read_manifest(record,bytes):-1;
    const char* const records[]={"receipt","receipt.new","retire","retire.new"};
    ok=ok && present>=0 && inventory(record,records,4);
    struct tree old={-1,-1,-1,-1,{-1,-1},-1,-1};int lease=-1;
    if(ok && !present){
        int parent=directory(store,"upgrade-generations"),prior=directory(parent,oldname),legacy=prior==-1;
        ok=parent>=0 && prior!=-2;
        if(prior>=0){close(prior);}if(parent>=0){close(parent);}
        ok=ok && open_tree(store,oldname,legacy,&old);
        struct pd_pair pinned={.lease=-1,.boot=-1,.payload=-1};struct pd_upgrade verified;
        ok=ok && pd_pair_acquire(old.control,old.stage,origin.predecessor.approval.candidate_key,arch,identity,&pinned) &&
            pinned.choice==(origin.predecessor.phase==PD_UPGRADE_COMMITTED?1:0) &&
            !flock(pinned.lease,LOCK_EX|LOCK_NB) &&
            pd_retained_reopen(store,old.stage,origin.predecessor.approval.candidate_key,arch,identity,&origin.predecessor,&verified) &&
            receipt_matches(old.record,&origin.predecessor,&verified);
        if(ok){lease=pinned.lease;pinned.lease=-1;memcpy(bytes,"PDRET001",8);bytes[8]=(unsigned char)legacy;
            memcpy(bytes+16,origin.record,sizeof(origin.record));}
        pd_pair_close(&pinned);
        /* Any inherited removal record must itself be bound to this predecessor. */
        unsigned char inherited[kSize];int inherited_present=ok && old.record>=0?read_manifest(old.record,inherited):0;
        ok=ok && inherited_present>=0 && (!inherited_present || manifest_valid(inherited,&origin.predecessor.approval));
        for(unsigned i=0;ok && i<kEntries;++i){int dir;const char* name;mode_t mode;member(&old,i,&dir,&name,&mode);
            ok=fingerprint(dir,name,mode,bytes+kHeader+i*kEntry,1);}
        if(ok){crypto_blake2b(bytes+kSize-32,32,bytes,kSize-32);}
        ok=ok && manifest_valid(bytes,&state.approval) && pd_capsule_io_copy(record,"retire",-1,bytes,kSize,0600);
    }else if(ok){
        ok=absent(record,"retire.new") && manifest_valid(bytes,&state.approval) && !memcmp(bytes+16,origin.record,sizeof(origin.record)) &&
            open_tree(store,oldname,bytes[8],&old);
        if(ok && old.control>=0){
            lease=openat(old.control,"lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
            ok=lease>=0?pd_capsule_io_file(lease,0,0600) && !flock(lease,LOCK_EX|LOCK_NB):
                errno==ENOENT && absent(old.control,"active") && old.stage==-1;
        }else if(ok){ok=old.control==-1 && old.stage==-1;}
    }
    int durable=ok?openat(record,"retire",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    ok=ok && durable>=0 && !fsync(durable) && !fsync(record) && pd_upgrade_checkpoint("retire-durable",0);
    if(durable>=0){close(durable);}
    /* Verify every remaining file before deletion, including after interruption. */
    for(unsigned i=0;ok && i<kEntries;++i){int dir;const char* name;mode_t mode;member(&old,i,&dir,&name,&mode);
        ok=fingerprint(dir,name,mode,bytes+kHeader+i*kEntry,0);}
    for(unsigned i=0;ok && i<kEntries;++i){
        if(i==20){continue;}int dir;const char* name;mode_t mode;member(&old,i,&dir,&name,&mode);
        if(dir<0){continue;}
        ok=(!unlinkat(dir,name,0) || errno==ENOENT) && !fsync(dir) && pd_upgrade_checkpoint("retire-file",i);
    }
    if(ok){ok=remove_directory(old.stage,"previous",0) && remove_directory(old.stage,"candidate",1) &&
        remove_directory(old.generation,bytes[8]?"upgrade-pair":"pair",2);}
    if(ok && old.control>=0){ok=(!unlinkat(old.control,"lock",0) || errno==ENOENT) && !fsync(old.control) &&
        pd_upgrade_checkpoint("retire-file",20);}
    if(ok){ok=remove_directory(old.generation,bytes[8]?"upgrade-control":"control",3) &&
        (bytes[8] || remove_directory(old.parent,oldname,4)) && remove_directory(old.receipts,oldname,5);}
    if(lease>=0){close(lease);}close_tree(&old);if(record>=0){close(record);}if(receipts>=0){close(receipts);}
    pd_pair_close(&selected);pd_generation_close(&current);return ok;
}
