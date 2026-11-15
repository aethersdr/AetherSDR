#define _GNU_SOURCE
#include "pd_slots_retire.h"
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_store.h"
#include "pd_bundle.h"
#include "pd_capsule_io.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char*,unsigned);
static uid_t owner(void){return getuid();}
#else
static int pd_upgrade_checkpoint(const char* p,unsigned i){(void)p;(void)i;return 1;}
static uid_t owner(void){return 0;}
#endif
enum { kMarker=8+PD_UPGRADE_APPROVAL_SIZE,kLength=kMarker+48,kSelector=kLength+4,
       kSlots=kSelector+PD_SELECTOR_SIZE,kChecksum=kSlots+3*40,kSize=PD_SLOTS_RETIRE_SIZE };
static const char* const names[]={"A.bundle","B.bundle","C.bundle"};
static uint32_t number(const unsigned char* p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void encode(unsigned char* p,uint32_t n){p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n;}
static int file(int dir,const char* name,off_t size)
{
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat a,b;
    if(fd<0){return errno==ENOENT?-1:-2;}
    if(fstat(fd,&a) || fstat(dir,&b) || a.st_dev!=b.st_dev || a.st_uid!=owner() || !S_ISREG(a.st_mode) ||
        a.st_nlink!=1 || (a.st_mode&07777)!=0600 || a.st_size!=size){close(fd);return -2;}return fd;
}
static int absent(int dir,const char* name)
{struct stat st;return fstatat(dir,name,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;}
static int inventory(int slots)
{
    int copy=openat(slots,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* list=copy>=0?fdopendir(copy):NULL;if(!list){if(copy>=0){close(copy);}return 0;}int ok=1;
    for(;;){errno=0;struct dirent* e=readdir(list);if(!e){ok=!errno;break;}
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")){continue;}
        unsigned i=0;for(;i<3 && strcmp(e->d_name,names[i]);++i){}if(i==3){ok=0;break;}}
    closedir(list);return ok;
}
static int marker(int store,uint32_t arch,uint32_t profile,unsigned char bytes[48])
{
    int fd=file(store,"installed",48);int ok=fd>=0 && pd_capsule_io_read(fd,bytes,48,0) &&
        !memcmp(bytes,"PDINST01",8) && number(bytes+8)==arch && number(bytes+12)==profile;
    if(fd>=0){close(fd);}return ok;
}
static int selection(int store,unsigned char bytes[PD_SELECTOR_SIZE],uint32_t* length,struct pd_selection* selected)
{
    struct stat st;if(fstatat(store,"active",&st,AT_SYMLINK_NOFOLLOW) || (st.st_size!=48 && st.st_size!=PD_SELECTOR_SIZE)){return 0;}
    int fd=file(store,"active",st.st_size);memset(bytes,0,PD_SELECTOR_SIZE);*length=(uint32_t)st.st_size;
    int ok=fd>=0 && pd_capsule_io_read(fd,bytes,*length,0) && pd_selection_read(store,selected)==1;
    if(fd>=0){close(fd);}return ok;
}
static int slot(int dir,unsigned index,const unsigned char key[32],uint32_t arch,
                 const struct pd_selection* selected,unsigned char record[40],int capture)
{
    struct stat st;
    if(fstatat(dir,names[index],&st,AT_SYMLINK_NOFOLLOW)){
        if(errno!=ENOENT){return 0;}
        if(capture){memset(record,0,40);return selected->current!='A'+index && selected->previous!='A'+index;}
        return 1;
    }
    if(st.st_size<PD_BUNDLE_PREFIX || st.st_size>PD_BUNDLE_MAX){return 0;}
    int fd=file(dir,names[index],st.st_size);struct pd_bundle bundle;unsigned char digest[32];
    int ok=fd>=0 && pd_bundle_hash(fd,digest) && pd_bundle_read(fd,key,arch,&bundle) && pd_bundle_validate(fd,&bundle);
    if(ok && selected->current=='A'+index){ok=!crypto_verify32(digest,selected->current_hash);}
    if(ok && selected->previous=='A'+index){ok=!crypto_verify32(digest,selected->previous_hash);}
    if(ok && capture){memset(record,0,40);record[0]=1;encode(record+4,(uint32_t)st.st_size);memcpy(record+8,digest,32);}
    else if(ok){ok=record[0]==1 && number(record+4)==st.st_size && !memcmp(record+8,digest,32);}
    if(fd>=0){close(fd);}return ok;
}
int pd_slots_retire(int store,const unsigned char key[32],uint32_t arch,const struct pd_identity* identity)
{
    struct pd_upgrade_state state;
    if(pd_upgrade_state_read(store,&state)!=1 ||
        (state.phase!=PD_UPGRADE_COMMITTED && state.phase!=PD_UPGRADE_ROLLED_BACK) ||
        memcmp(key,state.approval.candidate_key,32) || !pd_upgrade_at_phase(store,state.approval.record,state.phase)){return 0;}
    struct pd_generation generation;struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};
    int ok=pd_generation_active(store,&generation)==1 && !generation.legacy &&
        pd_pair_acquire(generation.control,generation.stage,key,arch,identity,&pair) &&
        pair.choice==(state.phase==PD_UPGRADE_COMMITTED?1:0) && absent(generation.control,"active.new") && absent(store,"active.new");
    int slots=ok?openat(store,"slots",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;struct stat a,b;
    ok=ok && slots>=0 && !fstat(slots,&a) && !fstat(store,&b) && a.st_dev==b.st_dev && a.st_uid==owner() &&
        S_ISDIR(a.st_mode) && ((a.st_mode&07777)==0700 || (a.st_mode&07777)==0755) && inventory(slots);
    unsigned char installed[48],selector[PD_SELECTOR_SIZE],bytes[kSize]={0};uint32_t length=0;struct pd_selection selected;
    ok=ok && marker(store,arch,state.approval.candidate.profile,installed) && selection(store,selector,&length,&selected);
    int receipt=ok?file(store,"slots-retired",kSize):-2,present=receipt>=0;
    ok=ok && receipt!=-2;
    if(ok && present){
        unsigned char digest[32];struct pd_upgrade captured;
        ok=pd_capsule_io_read(receipt,bytes,kSize,0) && absent(store,"slots-retired.new");
        crypto_blake2b(digest,32,bytes,kChecksum);
        ok=ok && !memcmp(bytes,"PDSLOT01",8) && !crypto_verify32(digest,bytes+kChecksum) &&
            pd_upgrade_parse(bytes+8,PD_UPGRADE_APPROVAL_SIZE,&captured) &&
            captured.candidate.profile==state.approval.candidate.profile && !strcmp(captured.candidate.serial,identity->serial) &&
            !memcmp(bytes+kMarker,installed,48) && number(bytes+kLength)==length && !memcmp(bytes+kSelector,selector,PD_SELECTOR_SIZE);
        for(unsigned i=0;ok && i<3;++i){const unsigned char* r=bytes+kSlots+i*40;
            ok=r[0]<=1 && !r[1] && !r[2] && !r[3] && number(r+4)<=PD_BUNDLE_MAX &&
                (r[0] || !memcmp(r,(unsigned char[40]){0},40));}
    }else if(ok){
        memcpy(bytes,"PDSLOT01",8);memcpy(bytes+8,state.approval.record,PD_UPGRADE_APPROVAL_SIZE);
        memcpy(bytes+kMarker,installed,48);encode(bytes+kLength,length);memcpy(bytes+kSelector,selector,PD_SELECTOR_SIZE);
        for(unsigned i=0;ok && i<3;++i){ok=slot(slots,i,installed+16,arch,&selected,bytes+kSlots+i*40,1);}
        if(ok){crypto_blake2b(bytes+kChecksum,32,bytes,kChecksum);}
        ok=ok && pd_capsule_io_copy(store,"slots-retired",-1,bytes,kSize,0600);
    }
    if(receipt>=0){close(receipt);}receipt=ok?file(store,"slots-retired",kSize):-1;
    ok=ok && receipt>=0 && !fsync(receipt) && !fsync(store) && pd_upgrade_checkpoint("slots-durable",0);
    if(receipt>=0){close(receipt);}
    for(unsigned i=0;ok && i<3;++i){ok=slot(slots,i,installed+16,arch,&selected,bytes+kSlots+i*40,0);}
    for(unsigned i=0;ok && i<3;++i){ok=(!unlinkat(slots,names[i],0) || errno==ENOENT) && !fsync(slots) &&
        pd_upgrade_checkpoint("slots-removed",i);}
    if(slots>=0){close(slots);}pd_pair_close(&pair);pd_generation_close(&generation);return ok;
}
