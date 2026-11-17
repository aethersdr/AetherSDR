#define _GNU_SOURCE
#include "pd_management.h"
#include "pd_store.h"
#include "pd_upgrade_state.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>
#ifdef PD_MANAGEMENT_TEST
static uid_t owner(void){return getuid();}
extern int pd_management_checkpoint(const char* point,unsigned index);
extern uint64_t pd_management_test_space(uint64_t actual);
#else
static uid_t owner(void){return 0;}
static int pd_management_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
static uint64_t pd_management_test_space(uint64_t actual){return actual;}
#endif
struct member {int directory;const char* name;const unsigned char* bytes;size_t size;mode_t mode;unsigned index;int system_unit;};
static int directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==owner() && !(st.st_mode&07022);
}
static int read_hash(int fd,size_t size,unsigned char digest[32])
{
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);unsigned char bytes[16384];size_t used=0;
    while(used<size){
        size_t count=size-used;if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(fd,bytes,count,(off_t)used);if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        crypto_blake2b_update(&hash,bytes,(size_t)n);used+=(size_t)n;
    }
    crypto_blake2b_final(&hash,digest);return 1;
}
static int same_filesystem(int fd,int parent,int system_unit,dev_t file_device,dev_t parent_device)
{
    if(file_device==parent_device){return 1;}
    if(!system_unit){return 0;}
    /* Overlayfs may report its backing device for a regular file. Only the
     * fixed system-unit directory accepts that case, on the same overlay fs.
     * https://kernel.org/doc/html/latest/filesystems/overlayfs.html */
    struct statfs file_fs,parent_fs;
    return !fstatfs(fd,&file_fs) && !fstatfs(parent,&parent_fs) &&
        file_fs.f_type==0x794c7630 && parent_fs.f_type==0x794c7630 &&
        !memcmp(&file_fs.f_fsid,&parent_fs.f_fsid,sizeof(file_fs.f_fsid));
}
static int metadata(int fd,int parent,mode_t mode,struct stat* st,int system_unit)
{
    struct stat dir;
    return fd>=0 && !fstat(fd,st) && !fstat(parent,&dir) && S_ISREG(st->st_mode) && st->st_uid==owner() &&
        st->st_nlink==1 && same_filesystem(fd,parent,system_unit,st->st_dev,dir.st_dev) &&
        (st->st_mode&07777)==mode && st->st_size>=0;
}
/* 1 previous, 2 desired, 0 foreign/missing. Atomic replacement never removes
 * the old pathname before the complete desired file has been synced. */
static int current(const struct member* member,const struct pd_release* previous)
{
    int fd=openat(member->directory,member->name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    int ok=metadata(fd,member->directory,member->mode,&st,member->system_unit),result=0;unsigned char digest[32],desired[32];
    if(ok && (st.st_size==(off_t)member->size || st.st_size==(off_t)previous->length[member->index]) &&
        read_hash(fd,(size_t)st.st_size,digest)){
        crypto_blake2b(desired,32,member->bytes,member->size);
        if(st.st_size==(off_t)member->size && !crypto_verify32(digest,desired)){result=2;}
        else if(st.st_size==(off_t)previous->length[member->index] &&
            !crypto_verify32(digest,previous->manifest+PD_RELEASE_HEADER+member->index*PD_RELEASE_RECORD+4)){result=1;}
    }
    if(fd>=0){close(fd);}return result;
}
static int partial(const struct member* member,const char* name)
{
    int fd=openat(member->directory,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    struct stat st;int ok=metadata(fd,member->directory,0600,&st,member->system_unit) ||
        (metadata(fd,member->directory,member->mode,&st,member->system_unit) && st.st_size==(off_t)member->size);
    ok=ok && st.st_size<=(off_t)member->size;unsigned char bytes[16384];size_t used=0;
    while(ok && used<(size_t)st.st_size){
        size_t count=(size_t)st.st_size-used;if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(fd,bytes,count,(off_t)used);if(n<0 && errno==EINTR){continue;}
        if(n<=0 || memcmp(bytes,member->bytes+used,(size_t)n)){ok=0;break;}used+=(size_t)n;
    }
    close(fd);return ok;
}
static int publish(const struct member* member,const struct pd_release* previous,unsigned index)
{
    char name[96];snprintf(name,sizeof(name),".%s.management-new",member->name);
    int state=current(member,previous);
    if(!state || !partial(member,name)){return 0;}
    if(state==2){
        return (!unlinkat(member->directory,name,0) || errno==ENOENT) && !fsync(member->directory);
    }
    int fd=openat(member->directory,name,O_RDWR|O_CREAT|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}struct stat st;
    int ok=metadata(fd,member->directory,0600,&st,member->system_unit) ||
        (metadata(fd,member->directory,member->mode,&st,member->system_unit) && st.st_size==(off_t)member->size);
    size_t used=0;
    while(ok && used<member->size){
        ssize_t n=pwrite(fd,member->bytes+used,member->size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fchmod(fd,member->mode) && !fsync(fd);if(close(fd)){ok=0;}
    return ok && pd_management_checkpoint("file",index) && current(member,previous) &&
        !renameat(member->directory,name,member->directory,member->name) && pd_management_checkpoint("rename",index) &&
        !fsync(member->directory) && pd_management_checkpoint("durable",index);
}
static int budget(const struct member members[10],const struct pd_release* previous)
{
    struct {dev_t device;uint64_t needed,available,block,inodes;int inode_known;} volumes[3];unsigned count=0;
    memset(volumes,0,sizeof(volumes));
    for(unsigned i=0;i<10;++i){
        struct stat st;struct statvfs fs;
        if(fstat(members[i].directory,&st) || fstatvfs(members[i].directory,&fs) || !fs.f_frsize ||
            (uint64_t)fs.f_bavail>UINT64_MAX/(uint64_t)fs.f_frsize){return 0;}
        unsigned v=0;for(;v<count && volumes[v].device!=st.st_dev;++v){}
        if(v==count){
            if(count==3){return 0;}++count;volumes[v].device=st.st_dev;volumes[v].block=fs.f_frsize;
            volumes[v].available=pd_management_test_space((uint64_t)fs.f_bavail*(uint64_t)fs.f_frsize);
            volumes[v].needed=PD_STORE_RESERVE;volumes[v].inode_known=fs.f_files!=0;volumes[v].inodes=fs.f_favail;
        }
        if(current(&members[i],previous)==2){continue;}
        uint64_t block=volumes[v].block,bytes=members[i].size;
        if(block>UINT64_MAX/2 || bytes>UINT64_MAX-(block-1)){return 0;}
        uint64_t allocation=((bytes+block-1)/block)*block;
        if(allocation>UINT64_MAX-block || volumes[v].needed>UINT64_MAX-allocation-block){return 0;}
        volumes[v].needed+=allocation+block;
        if(volumes[v].inode_known && !volumes[v].inodes--){return 0;}
    }
    for(unsigned v=0;v<count;++v){if(volumes[v].needed>volumes[v].available){return 0;}}
    return 1;
}
static int apply(int store,int units,const struct pd_upgrade* approved,const struct pd_management_blob* blob,
                   const struct pd_upgrade_origin* origin)
{
    struct pd_upgrade parsed;struct pd_release candidate,previous;unsigned char header[PD_RELEASE_PREFIX];
    if(!approved || !blob || !blob->boot || !blob->licenses ||
        !pd_upgrade_parse(approved->record,sizeof(approved->record),&parsed) ||
        memcmp(parsed.candidate.digest,blob->release_digest,32) || !directory(store) || !directory(units) ||
        !(pd_upgrade_at_phase(store,approved->record,PD_UPGRADE_QUIESCING) ||
          pd_upgrade_at_phase(store,approved->record,PD_UPGRADE_ROLLBACK))){return 0;}
    memcpy(header,approved->previous_release.manifest,PD_RELEASE_MANIFEST);
    memcpy(header+PD_RELEASE_MANIFEST,approved->previous_release.signature,64);
    uint32_t arch=approved->previous_release.arch;
    struct pd_upgrade_origin decoded;
    if(origin){
        if(!pd_upgrade_origin_parse(origin->record,sizeof(origin->record),&parsed,&decoded) || decoded.management.arch!=arch){return 0;}
        previous=decoded.management;
    }else if(!pd_release_header_parse(header,sizeof(header),parsed.previous_key,arch,parsed.previous.profile,parsed.previous.firmware,&previous)){return 0;}
    if(!pd_release_header_parse(blob->header,sizeof(blob->header),parsed.candidate_key,arch,parsed.candidate.profile,parsed.candidate.firmware,&candidate) ||
        !candidate.managed_startup || !pd_release_record_validate(blob->boot,blob->boot_size,0,&candidate) ||
        !pd_release_record_validate(blob->licenses,blob->license_size,3,&candidate)){return 0;}
    if(origin && !candidate.origin_recovery){return 0;}
    int cache=openat(store,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(!directory(cache)){if(cache>=0){close(cache);}return 0;}
    struct member members[10]={{store,"pd-boot",blob->boot,blob->boot_size,0755,0,0},
        {store,"BOOT-LICENSES.txt",blob->licenses,blob->license_size,0644,3,0}};
    int ok=1;
    for(unsigned i=0;ok && i<4;++i){
        const char* definition=pd_release_unit_text(&candidate,i);if(!definition){ok=0;break;}
        size_t size=strlen(definition);
        ok=pd_release_record_validate((const unsigned char*)definition,size,i+4,&candidate);
        members[i+2]=(struct member){cache,pd_release_names[i+4],(const unsigned char*)definition,size,0644,i+4,0};
        members[i+6]=members[i+2];members[i+6].directory=units;members[i+6].system_unit=1;
        char dropin[96];struct stat st;snprintf(dropin,sizeof(dropin),"%s.d",pd_release_names[i+4]);
        ok=ok && fstatat(units,dropin,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    }
    for(unsigned i=0;ok && i<10;++i){
        char name[96];snprintf(name,sizeof(name),".%s.management-new",members[i].name);
        ok=current(&members[i],&previous) && partial(&members[i],name);
    }
    ok=ok && budget(members,&previous) && pd_management_checkpoint("preflight",0);
    for(unsigned i=0;ok && i<10;++i){ok=publish(&members[i],&previous,i);}
    close(cache);return ok;
}
int pd_management_apply(int store,int units,const struct pd_upgrade* approved,const struct pd_management_blob* blob)
{
    return apply(store,units,approved,blob,NULL);
}
int pd_management_apply_origin(int store,int units,const struct pd_upgrade* approved,
                               const struct pd_management_blob* blob,const struct pd_upgrade_origin* origin)
{
    return origin && apply(store,units,approved,blob,origin);
}
