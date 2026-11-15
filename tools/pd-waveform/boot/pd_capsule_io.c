#define _GNU_SOURCE
#include "pd_capsule_io.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef PD_CAPSULE_TEST
static uid_t capsule_owner(void){return getuid();}
#else
static uid_t capsule_owner(void){return 0;}
#endif
#ifdef PD_CAPSULE_FAULTS
extern int pd_capsule_checkpoint(const char* point);
#else
static int pd_capsule_checkpoint(const char* point){(void)point;return 1;}
#endif
int pd_capsule_io_directory(int fd,int private)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==capsule_owner() &&
        !(st.st_mode&022) && (!private || (st.st_mode&07777)==0700);
}
int pd_capsule_io_file(int fd,off_t size,mode_t mode)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==capsule_owner() &&
        st.st_nlink==1 && (st.st_mode&07777)==mode && st.st_size==size;
}
int pd_capsule_io_read(int fd,unsigned char* bytes,size_t size,off_t offset)
{
    while(size){
        ssize_t n=pread(fd,bytes,size,offset);if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        bytes+=n;size-=(size_t)n;offset+=n;
    }
    return 1;
}
int pd_capsule_io_hash(int fd,off_t limit,unsigned char digest[32],off_t* size)
{
    struct stat before,after;
    if(fd<0 || fstat(fd,&before) || !S_ISREG(before.st_mode) || before.st_uid!=capsule_owner() ||
        before.st_nlink!=1 || (before.st_mode&06022) || before.st_size<=0 || before.st_size>limit){return 0;}
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);unsigned char bytes[16384];off_t offset=0;
    while(offset<before.st_size){
        size_t count=(size_t)(before.st_size-offset);if(count>sizeof(bytes)){count=sizeof(bytes);}
        if(!pd_capsule_io_read(fd,bytes,count,offset)){return 0;}
        crypto_blake2b_update(&hash,bytes,count);offset+=(off_t)count;
    }
    crypto_blake2b_final(&hash,digest);*size=before.st_size;
    return !fstat(fd,&after) && before.st_size==after.st_size && before.st_mtime==after.st_mtime && before.st_ctime==after.st_ctime;
}
int pd_capsule_io_equal(int fd,int source,const unsigned char* memory,off_t count)
{
    unsigned char actual[16384],expected[16384];off_t offset=0;
    while(offset<count){
        size_t n=(size_t)(count-offset);if(n>sizeof(actual)){n=sizeof(actual);}
        if(!pd_capsule_io_read(fd,actual,n,offset)){return 0;}
        if(memory){memcpy(expected,memory+offset,n);}else if(!pd_capsule_io_read(source,expected,n,offset)){return 0;}
        if(memcmp(actual,expected,n)){return 0;}offset+=(off_t)n;
    }
    return 1;
}
int pd_capsule_io_existing(int directory,const char* name,int source,const unsigned char* memory,
                             off_t size,mode_t mode,off_t* present)
{
    *present=0;char temporary[32];snprintf(temporary,sizeof(temporary),"%s.new",name);struct stat st;
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0){
        int ok=pd_capsule_io_file(fd,size,mode) && pd_capsule_io_equal(fd,source,memory,size);close(fd);
        ok=ok && fstatat(directory,temporary,&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
        if(ok){*present=size;}return ok;
    }
    if(errno!=ENOENT){return 0;}
    fd=openat(directory,temporary,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=size &&
        (pd_capsule_io_file(fd,st.st_size,0600) || (st.st_size==size && pd_capsule_io_file(fd,size,mode))) &&
        pd_capsule_io_equal(fd,source,memory,st.st_size);
    if(ok){*present=st.st_size;}close(fd);return ok;
}
int pd_capsule_io_copy(int directory,const char* name,int source,const unsigned char* memory,off_t size,mode_t mode)
{
    off_t present=0;if(!pd_capsule_io_existing(directory,name,source,memory,size,mode,&present)){return 0;}
    struct stat st;if(!fstatat(directory,name,&st,AT_SYMLINK_NOFOLLOW)){return 1;}if(errno!=ENOENT){return 0;}
    char temporary[32];snprintf(temporary,sizeof(temporary),"%s.new",name);
    int output=openat(directory,temporary,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(output<0){return 0;}
    int ok=(pd_capsule_io_file(output,present,0600) || (present==size && pd_capsule_io_file(output,size,mode))) &&
        pd_capsule_io_equal(output,source,memory,present);
    unsigned char bytes[16384];off_t offset=present;
    while(ok && offset<size){
        size_t count=(size_t)(size-offset);if(count>sizeof(bytes)){count=sizeof(bytes);}
        if(memory){memcpy(bytes,memory+offset,count);}else if(!pd_capsule_io_read(source,bytes,count,offset)){ok=0;break;}
        size_t used=0;
        while(used<count){
            ssize_t n=pwrite(output,bytes+used,count-used,offset+(off_t)used);
            if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
        }
        offset+=(off_t)used;
        char point[48];snprintf(point,sizeof(point),"%s-bytes",name);
        ok=ok && pd_capsule_checkpoint(point);
    }
    ok=ok && !fchmod(output,mode) && !fsync(output);if(close(output)){ok=0;}
    char point[48];snprintf(point,sizeof(point),"%s-file",name);
    ok=ok && pd_capsule_checkpoint(point) && !renameat(directory,temporary,directory,name);
    snprintf(point,sizeof(point),"%s-rename",name);
    return ok && pd_capsule_checkpoint(point) && !fsync(directory);
}
