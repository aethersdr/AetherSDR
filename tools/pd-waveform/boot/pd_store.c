#define _GNU_SOURCE
#include "pd_store.h"
#include "pd_bundle.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef PD_STORE_TEST
/* Test-only adapter supplies fault injection and fixture ownership. */
extern int pd_store_checkpoint(const char* point);
extern uint64_t pd_store_test_space(uint64_t actual);
static uid_t store_owner(void){return getuid();}
#else
static int pd_store_checkpoint(const char* point){(void)point;return 1;}
static uint64_t pd_store_test_space(uint64_t actual){return actual;}
static uid_t store_owner(void){return 0;}
#endif
static int store_directory(int fd)
{
    struct stat st;return !fstat(fd,&st) && S_ISDIR(st.st_mode) &&
        st.st_uid==store_owner() && !(st.st_mode&022);
}
static int store_file(int directory,const char* name)
{
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);struct stat st;
    if(fd<0){return -1;}
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=store_owner() ||
        st.st_nlink!=1 || (st.st_mode&06022)){close(fd);errno=EINVAL;return -1;}
    return fd;
}
static int store_write(int fd,const void* bytes,size_t count)
{
    const unsigned char* p=bytes;
    while(count){
        ssize_t n=write(fd,p,count);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        p+=n;count-=(size_t)n;
    }
    return 1;
}
static const char* slot_name(unsigned char slot)
{
    if(slot=='A'){return "A.bundle";}if(slot=='B'){return "B.bundle";}
    if(slot=='C'){return "C.bundle";}return NULL;
}
int pd_store_slot(int slots,unsigned char slot)
{
    const char* name=slot_name(slot);return name?store_file(slots,name):-1;
}
int pd_selection_read(int store,struct pd_selection* selected)
{
    memset(selected,0,sizeof(*selected));
    int fd=store_file(store,"active");if(fd<0){return errno==ENOENT?0:-1;}
    unsigned char bytes[PD_SELECTOR_SIZE+1];ssize_t n=pread(fd,bytes,sizeof(bytes),0);close(fd);
    static const unsigned char zero[32]={0};
    int legacy=n==48 && !memcmp(bytes,"PDSEL001",8);
    if(!legacy && (n!=PD_SELECTOR_SIZE || memcmp(bytes,"PDSEL002",8))){return -1;}
    if(!slot_name(bytes[8]) || memcmp(bytes+9,zero,7) ||
        (legacy && bytes[8]=='C')){return -1;}
    selected->current=bytes[8];memcpy(selected->current_hash,bytes+16,32);
    if(!legacy){
        if(memcmp(bytes+49,zero,7) ||
            (bytes[48] && (!slot_name(bytes[48]) || bytes[48]==bytes[8])) ||
            (!bytes[48] && memcmp(bytes+56,zero,32))){return -1;}
        selected->previous=bytes[48];memcpy(selected->previous_hash,bytes+56,32);
    }
    return 1;
}
int pd_store_lock(int store)
{
    if(!store_directory(store)){return -1;}
    int fd=openat(store,"store.lock",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    struct stat st;
    if(fd<0){return -1;}
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=store_owner() ||
        st.st_nlink!=1 || (st.st_mode&07777)!=0600 || flock(fd,LOCK_EX|LOCK_NB)){
        close(fd);return -1;
    }
    return fd;
}
static int verified_slot(int slots,unsigned char slot,const unsigned char digest[32],
                         const unsigned char key[32],uint32_t arch)
{
    int fd=pd_store_slot(slots,slot);if(fd<0){return 0;}
    struct pd_bundle bundle;unsigned char actual[32];
    int ok=pd_bundle_hash(fd,actual) && (!digest || !crypto_verify32(actual,digest)) &&
        pd_bundle_read(fd,key,arch,&bundle) && pd_bundle_validate(fd,&bundle);
    close(fd);return ok;
}
static int remove_temporary(int directory,const char* name,off_t maximum)
{
    int fd=store_file(directory,name);if(fd<0){return errno==ENOENT;}
    struct stat st;int ok=!fstat(fd,&st) && (st.st_mode&07777)==0600 &&
        st.st_size>=0 && st.st_size<=maximum;close(fd);
    return ok && !unlinkat(directory,name,0) && !fsync(directory);
}
static int collect_slots(int slots,const struct pd_selection* keep,
                         const unsigned char key[32],uint32_t arch)
{
    for(unsigned char slot='A';slot<='C';++slot){
        if(slot==keep->current || slot==keep->previous){continue;}
        struct stat st;
        if(fstatat(slots,slot_name(slot),&st,AT_SYMLINK_NOFOLLOW)){
            if(errno==ENOENT){continue;}return 0;
        }
        if(!verified_slot(slots,slot,NULL,key,arch) || unlinkat(slots,slot_name(slot),0)){return 0;}
    }
    return !fsync(slots);
}
static int publish_selection(int store,const struct pd_selection* selected)
{
    unsigned char bytes[PD_SELECTOR_SIZE]={0};memcpy(bytes,"PDSEL002",8);
    bytes[8]=selected->current;memcpy(bytes+16,selected->current_hash,32);
    bytes[48]=selected->previous;memcpy(bytes+56,selected->previous_hash,32);
    int fd=openat(store,"active.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}
    int ok=store_write(fd,bytes,sizeof(bytes)) && pd_store_checkpoint("selector-written") &&
        !fsync(fd) && pd_store_checkpoint("selector-synced");
    if(close(fd)){ok=0;}
    if(!ok || renameat(store,"active.new",store,"active") ||
        !pd_store_checkpoint("selector-renamed") || fsync(store) ||
        !pd_store_checkpoint("selector-committed")){return 0;}
    return 1;
}
static int open_slots(int store)
{
    if(!store_directory(store)){return -1;}
    int slots=openat(store,"slots",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(slots>=0 && !store_directory(slots)){close(slots);return -1;}
    return slots;
}
static int copy_input(int input,int output,off_t size)
{
    unsigned char bytes[16384];off_t offset=0;
    while(offset<size){
        size_t count=size-offset<(off_t)sizeof(bytes)?(size_t)(size-offset):sizeof(bytes);
        ssize_t n=pread(input,bytes,count,offset);
        if(n<0 && errno==EINTR){continue;}
        if(n<=0 || !store_write(output,bytes,(size_t)n)){return 0;}offset+=n;
    }
    return 1;
}
static int enough_space(int slots,off_t incoming)
{
    struct statvfs st;if(fstatvfs(slots,&st) || !st.f_frsize){return 0;}
    uint64_t available=(uint64_t)st.f_bavail>UINT64_MAX/st.f_frsize?UINT64_MAX:
        (uint64_t)st.f_bavail*st.f_frsize;
    return pd_store_test_space(available)>=(uint64_t)incoming+PD_STORE_RESERVE;
}
static int original_slots_live(int store)
{
    struct stat st;
    return fstatat(store,"slots-retired",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        fstatat(store,"slots-retired.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
}
int pd_store_update(int store,int input,const unsigned char key[32],uint32_t arch)
{
    if(!original_slots_live(store)){return 0;}
    struct pd_bundle bundle;struct stat st;struct pd_selection current,next;
    int state=pd_selection_read(store,&current);
    if(state<0 || fstat(input,&st) || !S_ISREG(st.st_mode) ||
        st.st_size<PD_BUNDLE_PREFIX || st.st_size>PD_BUNDLE_MAX ||
        !pd_bundle_read(input,key,arch,&bundle)){return 0;}
    int slots=open_slots(store);if(slots<0){return 0;}
    int ok=(!current.current || verified_slot(slots,current.current,current.current_hash,key,arch)) &&
        (!current.previous || verified_slot(slots,current.previous,current.previous_hash,key,arch));
    unsigned char digest[32];
    if(ok && pd_bundle_hash(input,digest) && current.current && !crypto_verify32(digest,current.current_hash)){
        /* A previous process may have died after selector rename but before
         * its directory sync. Make that selection durable before collection. */
        ok=remove_temporary(slots,"incoming.new",PD_BUNDLE_MAX) &&
            remove_temporary(store,"active.new",PD_SELECTOR_SIZE) &&
            pd_store_checkpoint("retry-selector-observed") && !fsync(store) &&
            collect_slots(slots,&current,key,arch);
        close(slots);return ok;
    }
    ok=ok && remove_temporary(slots,"incoming.new",PD_BUNDLE_MAX) &&
        remove_temporary(store,"active.new",PD_SELECTOR_SIZE) && collect_slots(slots,&current,key,arch) &&
        enough_space(slots,st.st_size);
    if(!ok){close(slots);return 0;}
    unsigned char target='A';while(target==current.current || target==current.previous){++target;}
    int output=openat(slots,"incoming.new",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(output<0){close(slots);return 0;}
    ok=copy_input(input,output,st.st_size) && pd_store_checkpoint("payload-written") &&
        pd_bundle_read(output,key,arch,&bundle) && pd_bundle_validate(output,&bundle) &&
        pd_bundle_hash(output,digest) && !fsync(output) && pd_store_checkpoint("payload-synced");
    if(close(output)){ok=0;}
    if(ok){ok=!renameat(slots,"incoming.new",slots,slot_name(target)) &&
        pd_store_checkpoint("payload-renamed") && !fsync(slots) && pd_store_checkpoint("payload-committed");}
    if(ok){
        memset(&next,0,sizeof(next));next.current=target;memcpy(next.current_hash,digest,32);
        next.previous=current.current;memcpy(next.previous_hash,current.current_hash,32);
        ok=publish_selection(store,&next);
    }
    if(ok){ok=collect_slots(slots,&next,key,arch) && pd_store_checkpoint("retired-collected");}
    close(slots);return ok;
}
int pd_store_rollback(int store,const unsigned char key[32],uint32_t arch)
{
    if(!original_slots_live(store)){return 0;}
    struct pd_selection current,next;
    if(pd_selection_read(store,&current)!=1 || !current.previous){return 0;}
    int slots=open_slots(store);if(slots<0){return 0;}
    int ok=verified_slot(slots,current.previous,current.previous_hash,key,arch);
    memset(&next,0,sizeof(next));next.current=current.previous;memcpy(next.current_hash,current.previous_hash,32);
    if(verified_slot(slots,current.current,current.current_hash,key,arch)){
        next.previous=current.current;memcpy(next.previous_hash,current.current_hash,32);
    }
    ok=ok && remove_temporary(store,"active.new",PD_SELECTOR_SIZE) && publish_selection(store,&next);
    /* A corrupt former current slot is retained for inspection, never silently
     * treated as a valid rollback or deleted by garbage collection. */
    close(slots);return ok;
}
