#define _GNU_SOURCE
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_capsule_io.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
static void initialize(struct pd_generation* generation)
{
    memset(generation,0,sizeof(*generation));generation->directory=generation->stage=generation->control=-1;
}
void pd_generation_close(struct pd_generation* generation)
{
    if(generation->stage>=0){close(generation->stage);}if(generation->control>=0){close(generation->control);}
    if(generation->directory>=0){close(generation->directory);}initialize(generation);
}
static int directory_ok(int fd,int private)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==owner() &&
        (private?(st.st_mode&07777)==0700:!(st.st_mode&07022));
}
static int child(int parent,const char* name,int create)
{
    if(!directory_ok(parent,0)){return -1;}
    if(create && mkdirat(parent,name,0700) && errno!=EEXIST){return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat a,b;
    int ok=directory_ok(fd,1) && !fstat(parent,&a) && !fstat(fd,&b) && a.st_dev==b.st_dev && (!create || !fsync(parent));
    if(!ok){if(fd>=0){close(fd);}return -1;}return fd;
}
static int inventory(int directory)
{
    int fd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* entries=fd>=0?fdopendir(fd):NULL;if(!entries){if(fd>=0){close(fd);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* item=readdir(entries);if(!item){ok=!errno;break;}
        const char* name=item->d_name;
        if(strcmp(name,".") && strcmp(name,"..") && strcmp(name,"pair") && strcmp(name,"control")){ok=0;break;}
    }
    closedir(entries);return ok;
}
static void name_for(const unsigned char digest[32],char name[65])
{
    static const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){name[i*2]=hex[digest[i]>>4];name[i*2+1]=hex[digest[i]&15];}name[64]=0;
}
static int open_digest(int store,const unsigned char digest[32],int create,struct pd_generation* generation)
{
    initialize(generation);char name[65];name_for(digest,name);
    int parent=child(store,"upgrade-generations",create);
    generation->directory=parent>=0?child(parent,name,create):-1;if(parent>=0){close(parent);}
    int ok=generation->directory>=0 && inventory(generation->directory);
    generation->stage=ok?child(generation->directory,"pair",create):-1;
    generation->control=generation->stage>=0?child(generation->directory,"control",create):-1;
    ok=ok && generation->stage>=0 && generation->control>=0;
    if(!ok){pd_generation_close(generation);}return ok;
}
static void record(const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],unsigned char bytes[PD_GENERATION_RECORD_SIZE])
{
    memcpy(bytes,"PDGEN001",8);crypto_blake2b(bytes+8,32,approval,PD_UPGRADE_APPROVAL_SIZE);
}
static int read_record(int store,const char* name,unsigned char bytes[PD_GENERATION_RECORD_SIZE],size_t* size,int partial)
{
    *size=0;if(!directory_ok(store,0)){return -1;}
    int fd=openat(store,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    struct stat st,parent;
    int ok=!fstat(fd,&st) && !fstat(store,&parent) && S_ISREG(st.st_mode) && st.st_uid==owner() &&
        st.st_nlink==1 && st.st_dev==parent.st_dev && (st.st_mode&07777)==0600 &&
        st.st_size>=0 && st.st_size<=PD_GENERATION_RECORD_SIZE && (partial || st.st_size==PD_GENERATION_RECORD_SIZE);
    *size=ok?(size_t)st.st_size:0;
    ok=ok && pd_capsule_io_read(fd,bytes,*size,0);close(fd);return ok?1:-1;
}
static int stage_approval(int stage,unsigned char bytes[PD_UPGRADE_APPROVAL_SIZE])
{
    int fd=openat(stage,"approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat a,b;struct pd_upgrade parsed;
    int ok=fd>=0 && !fstat(fd,&a) && !fstat(stage,&b) && a.st_dev==b.st_dev &&
        S_ISREG(a.st_mode) && a.st_uid==owner() && a.st_nlink==1 && (a.st_mode&07777)==0600 &&
        a.st_size==PD_UPGRADE_APPROVAL_SIZE && pd_capsule_io_read(fd,bytes,PD_UPGRADE_APPROVAL_SIZE,0) &&
        pd_upgrade_parse(bytes,PD_UPGRADE_APPROVAL_SIZE,&parsed);
    if(fd>=0){close(fd);}return ok;
}
static int legacy(int store,struct pd_generation* generation)
{
    initialize(generation);struct stat a,b;
    int first=fstatat(store,"upgrade-pair",&a,AT_SYMLINK_NOFOLLOW),first_error=errno;
    int second=fstatat(store,"upgrade-control",&b,AT_SYMLINK_NOFOLLOW),second_error=errno;
    if(first<0 && second<0 && first_error==ENOENT && second_error==ENOENT){return 0;}
    if(first<0 || second<0){return -1;}
    generation->stage=child(store,"upgrade-pair",0);generation->control=child(store,"upgrade-control",0);
    if(generation->stage<0 || generation->control<0){pd_generation_close(generation);return -1;}
    generation->legacy=1;return 1;
}
int pd_generation_open(int store,const struct pd_upgrade* approved,int create,struct pd_generation* generation)
{
    struct pd_upgrade parsed;unsigned char bytes[PD_GENERATION_RECORD_SIZE];initialize(generation);
    if((create!=0 && create!=1) || !pd_upgrade_parse(approved->record,sizeof(approved->record),&parsed)){return 0;}
    record(parsed.record,bytes);return open_digest(store,bytes+8,create,generation);
}
int pd_generation_active(int store,struct pd_generation* generation)
{
    initialize(generation);unsigned char bytes[PD_GENERATION_RECORD_SIZE],pending[PD_GENERATION_RECORD_SIZE];size_t size;
    if(read_record(store,"upgrade-generation.new",pending,&size,1)!=0){return -1;}
    int present=read_record(store,"upgrade-generation",bytes,&size,0);
    if(present<0){return -1;}
    int opened=present?(!memcmp(bytes,"PDGEN001",8) && open_digest(store,bytes+8,0,generation)?1:-1):legacy(store,generation);
    if(opened!=1){
        struct pd_upgrade_state state;
        return opened==0 && pd_upgrade_state_read(store,&state)!=0?-1:opened;
    }
    unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],expected[PD_GENERATION_RECORD_SIZE];struct pd_upgrade_state state;
    int ok=stage_approval(generation->stage,approval),journal=ok?pd_upgrade_state_read(store,&state):-1;
    if(ok){record(approval,expected);}
    ok=ok && journal>=0 && (!present || (journal==1 && !memcmp(bytes,expected,sizeof(bytes)))) &&
        (journal==0 || !memcmp(state.approval.record,approval,sizeof(approval)));
    if(!ok){pd_generation_close(generation);}return ok?1:-1;
}
int pd_generation_select(int store,const struct pd_upgrade* approved,const struct pd_upgrade_origin* origin,
                          uint32_t arch,const struct pd_identity* identity)
{
    struct pd_upgrade_origin decoded;
    if(!origin || !pd_upgrade_origin_parse(origin->record,sizeof(origin->record),approved,&decoded) ||
        !(pd_upgrade_at_phase(store,approved->record,PD_UPGRADE_QUIESCING) ||
          pd_upgrade_at_phase(store,approved->record,PD_UPGRADE_ROLLBACK))){return 0;}
    unsigned char desired[PD_GENERATION_RECORD_SIZE],previous[PD_GENERATION_RECORD_SIZE],active[PD_GENERATION_RECORD_SIZE],pending[PD_GENERATION_RECORD_SIZE];
    record(approved->record,desired);if(decoded.has_predecessor){record(decoded.predecessor.approval.record,previous);}
    struct pd_generation generation;struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};
    unsigned char staged_approval[PD_UPGRADE_APPROVAL_SIZE];
    int ok=pd_generation_open(store,approved,0,&generation) &&
        stage_approval(generation.stage,staged_approval) && !memcmp(staged_approval,approved->record,sizeof(staged_approval)) &&
        pd_pair_acquire(generation.control,generation.stage,approved->candidate_key,arch,identity,&pair) && pair.choice==PD_PAIR_PREVIOUS;
    size_t size=0;int present=ok?read_record(store,"upgrade-generation",active,&size,0):-1;
    ok=ok && present>=0;
    if(ok && present){ok=!memcmp(active,desired,sizeof(active)) ||
        (decoded.has_predecessor && !memcmp(active,previous,sizeof(active)));}
    if(ok && !present){
        struct pd_generation old;int prior=legacy(store,&old);unsigned char approval[PD_UPGRADE_APPROVAL_SIZE];
        ok=decoded.has_predecessor?prior==1 && stage_approval(old.stage,approval) &&
            !memcmp(approval,decoded.predecessor.approval.record,sizeof(approval)):prior==0;
        pd_generation_close(&old);
    }
    int partial=ok?read_record(store,"upgrade-generation.new",pending,&size,1):-1;
    ok=ok && partial>=0 && (!partial || !memcmp(pending,desired,size));
    if(ok && partial){ok=!unlinkat(store,"upgrade-generation.new",0) && !fsync(store);}
    if(ok && present && !memcmp(active,desired,sizeof(active))){ok=!fsync(store);}
    else if(ok){
        int fd=openat(store,"upgrade-generation.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);size_t used=0;
        ok=fd>=0;
        while(ok && used<sizeof(desired)){
            ssize_t n=write(fd,desired+used,sizeof(desired)-used);
            if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
        }
        ok=ok && !fsync(fd) && pd_upgrade_checkpoint("generation-file",0);if(fd>=0 && close(fd)){ok=0;}
        ok=ok && !renameat(store,"upgrade-generation.new",store,"upgrade-generation") &&
            pd_upgrade_checkpoint("generation-rename",0) && !fsync(store) && pd_upgrade_checkpoint("generation-durable",0);
    }
    pd_pair_close(&pair);pd_generation_close(&generation);return ok;
}
