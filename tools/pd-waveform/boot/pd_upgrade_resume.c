#define _GNU_SOURCE
#include "pd_upgrade.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct snapshot { unsigned char* bytes;size_t size;struct pd_release release; };
#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char* point,unsigned index);
static uid_t resume_owner(void){return getuid();}
#else
static int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
static uid_t resume_owner(void){return 0;}
#endif
static int read_snapshot(int fd,const unsigned char key[32],uint32_t arch,
                         const struct pd_approval* approval,struct snapshot* snapshot)
{
    struct stat st;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<PD_RELEASE_PREFIX || st.st_size>PD_RELEASE_MAX){return 0;}
    snapshot->size=(size_t)st.st_size;snapshot->bytes=malloc(snapshot->size);if(!snapshot->bytes){return 0;}
    size_t used=0;
    while(used<snapshot->size){
        ssize_t n=pread(fd,snapshot->bytes+used,snapshot->size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    unsigned char digest[32];crypto_blake2b(digest,32,snapshot->bytes,snapshot->size);
    struct pd_release* release=&snapshot->release;
    if(crypto_verify32(digest,approval->digest) ||
        !pd_release_header_parse(snapshot->bytes,PD_RELEASE_PREFIX,key,arch,approval->profile,approval->firmware,release) ||
        (uint64_t)release->offset[PD_RELEASE_COUNT-1]+release->length[PD_RELEASE_COUNT-1]!=snapshot->size){return 0;}
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        if(!pd_release_record_validate(snapshot->bytes+release->offset[i],release->length[i],i,release)){return 0;}
    }
    return 1;
}
static int same_file(const struct stat* a,const struct stat* b)
{
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_uid==b->st_uid && a->st_gid==b->st_gid &&
        a->st_mode==b->st_mode && a->st_size==b->st_size && a->st_nlink==b->st_nlink;
}
static int prefix(int directory,const char* name,const unsigned char* bytes,size_t size,mode_t mode,
                   dev_t device,struct stat* observed)
{
    memset(observed,0,sizeof(*observed));
    if(fstatat(directory,name,observed,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT;}
    mode_t actual=observed->st_mode&07777;
    if(!S_ISREG(observed->st_mode) || observed->st_uid!=resume_owner() || observed->st_nlink!=1 ||
        observed->st_dev!=device || observed->st_size<0 || (uint64_t)observed->st_size>size ||
        (actual!=0600 && (actual!=mode || (uint64_t)observed->st_size!=size))){return 0;}
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat pinned;
    int ok=fd>=0 && !fstat(fd,&pinned) && same_file(observed,&pinned);size_t used=0;unsigned char buffer[16384];
    while(ok && used<(size_t)observed->st_size){
        size_t count=(size_t)observed->st_size-used;if(count>sizeof(buffer)){count=sizeof(buffer);}
        ssize_t n=pread(fd,buffer,count,(off_t)used);
        if(n<0 && errno==EINTR){continue;}
        if(n<=0 || memcmp(buffer,bytes+used,(size_t)n)){ok=0;break;}used+=(size_t)n;
    }
    if(fd>=0){close(fd);}return ok;
}
static int inventory(int directory,int top,unsigned* seen)
{
    *seen=0;const char* const names[]={"approved","previous","candidate"};unsigned count=top?3:PD_RELEASE_COUNT+1;
    int fd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=fd>=0?fdopendir(fd):NULL;if(!listing){if(fd>=0){close(fd);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(listing);if(!entry){ok=!errno;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        unsigned i=0;
        for(;i<count;++i){
            const char* name=top?names[i]:i<PD_RELEASE_COUNT?pd_release_names[i]:"release.header";
            if(!strcmp(entry->d_name,name)){break;}
        }
        if(i==count || (*seen&(1u<<i))){ok=0;break;}*seen|=1u<<i;
    }
    closedir(listing);return ok;
}
static int open_directory(int stage,const char* name,dev_t device,int* fd)
{
    *fd=-1;struct stat st,pinned;
    if(fstatat(stage,name,&st,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT;}
    if(!S_ISDIR(st.st_mode) || st.st_uid!=resume_owner() || st.st_dev!=device || (st.st_mode&07777)!=0700){return 0;}
    *fd=openat(stage,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    return *fd>=0 && !fstat(*fd,&pinned) && st.st_dev==pinned.st_dev && st.st_ino==pinned.st_ino &&
        st.st_uid==pinned.st_uid && st.st_mode==pinned.st_mode;
}
static int check_directory(int directory,dev_t device,const struct snapshot* snapshot)
{
    unsigned seen;struct stat observed;
    if(!inventory(directory,0,&seen) ||
        !prefix(directory,"release.header",snapshot->bytes,PD_RELEASE_PREFIX,0600,device,&observed)){return 0;}
    if((seen&((1u<<PD_RELEASE_COUNT)-1)) && (observed.st_nlink!=1 || observed.st_size!=PD_RELEASE_PREFIX)){return 0;}
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        if(!prefix(directory,pd_release_names[i],snapshot->bytes+snapshot->release.offset[i],snapshot->release.length[i],
            i<2?0755:0644,device,&observed)){return 0;}
    }
    return 1;
}
static int finish_file(int directory,const char* name,const unsigned char* bytes,size_t size,mode_t mode,dev_t device,unsigned index)
{
    struct stat observed,pinned;
    if(!prefix(directory,name,bytes,size,mode,device,&observed)){return 0;}
    int exists=observed.st_nlink==1;
    int fd=openat(directory,name,O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|(exists?0:O_CREAT|O_EXCL),0600);
    if(fd<0){return 0;}
    int ok=!fstat(fd,&pinned) && (exists?same_file(&observed,&pinned):
        S_ISREG(pinned.st_mode) && pinned.st_uid==resume_owner() && pinned.st_nlink==1 && pinned.st_dev==device && !pinned.st_size);
    size_t used=exists?(size_t)observed.st_size:0;
    while(ok && used<size){
        size_t count=size-used;if(count>16384){count=16384;}
        ssize_t n=pwrite(fd,bytes+used,count,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
        ok=pd_upgrade_checkpoint("resume-copy",index);
    }
    ok=ok && !fchmod(fd,mode) && !fsync(fd) && pd_upgrade_checkpoint("resume-file-synced",index);
    if(close(fd)){ok=0;}return ok;
}
int pd_upgrade_resume(int stage,int previous,int candidate,const unsigned char expected_key[32],uint32_t arch,
                      const struct pd_identity* identity,const struct pd_upgrade* approved)
{
    struct pd_upgrade parsed;struct stat stage_stat,observed;
    if(!pd_upgrade_parse(approved->record,sizeof(approved->record),&parsed) ||
        crypto_verify32(expected_key,parsed.candidate_key) ||
        !pd_identity_matches(identity,parsed.candidate.profile,parsed.candidate.firmware,parsed.candidate.serial) ||
        fstat(stage,&stage_stat) || !S_ISDIR(stage_stat.st_mode) || stage_stat.st_uid!=resume_owner() || (stage_stat.st_mode&07022)){return 0;}
    struct snapshot snapshots[2]={{0}};int directories[2]={-1,-1};unsigned seen=0;
    int ok=read_snapshot(previous,parsed.previous_key,arch,&parsed.previous,&snapshots[0]) &&
        read_snapshot(candidate,parsed.candidate_key,arch,&parsed.candidate,&snapshots[1]) &&
        pd_upgrade_checkpoint("resume-snapshots-verified",0) && inventory(stage,1,&seen) &&
        prefix(stage,"approved",parsed.record,sizeof(parsed.record),0600,stage_stat.st_dev,&observed);
    ok=ok && (!(seen&6) || (observed.st_nlink==1 && observed.st_size==PD_UPGRADE_APPROVAL_SIZE));
    const char* const names[]={"previous","candidate"};
    for(unsigned i=0;ok && i<2;++i){
        ok=open_directory(stage,names[i],stage_stat.st_dev,&directories[i]) &&
            (directories[i]<0 || check_directory(directories[i],stage_stat.st_dev,&snapshots[i]));
    }
    /* Inspect the complete existing tree before finishing even its first file. */
    struct pd_upgrade_budget budget;
    if(ok){ok=pd_upgrade_space(stage,&snapshots[0].release,&snapshots[1].release,&budget);}
    if(ok){ok=finish_file(stage,"approved",parsed.record,sizeof(parsed.record),0600,stage_stat.st_dev,0) && !fsync(stage);}
    for(unsigned i=0;ok && i<2;++i){
        if(directories[i]<0){
            ok=!mkdirat(stage,names[i],0700) && open_directory(stage,names[i],stage_stat.st_dev,&directories[i]);
        }
        const struct snapshot* snapshot=&snapshots[i];unsigned base=1+i*(PD_RELEASE_COUNT+1);
        ok=ok && finish_file(directories[i],"release.header",snapshot->bytes,PD_RELEASE_PREFIX,0600,stage_stat.st_dev,base);
        for(unsigned j=0;ok && j<PD_RELEASE_COUNT;++j){
            ok=finish_file(directories[i],pd_release_names[j],snapshot->bytes+snapshot->release.offset[j],snapshot->release.length[j],
                j<2?0755:0644,stage_stat.st_dev,base+1+j);
        }
        if(ok){ok=!fsync(directories[i]);}
    }
    for(unsigned i=0;i<2;++i){if(directories[i]>=0){close(directories[i]);}free(snapshots[i].bytes);}
    struct pd_upgrade restored;
    return ok && pd_upgrade_reopen(stage,expected_key,arch,identity,&restored) && !memcmp(restored.record,parsed.record,sizeof(parsed.record));
}
