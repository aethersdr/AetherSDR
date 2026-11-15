#define _GNU_SOURCE
#include <errno.h>
#include "pd_management.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
static const char* fault;
static char unit_path[4096];
int __real_fstat(int fd,struct stat* st);
int __real_fstatfs(int fd,struct statfs* st);
static int unit_descriptor(int fd)
{
    char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
    ssize_t size=readlink(link,path,sizeof(path)-1);if(size<0){return 0;}path[size]=0;
    size_t n=strlen(unit_path);return !strncmp(path,unit_path,n) && (path[n]==0 || path[n]=='/');
}
static int device_fixture(void)
{
    return !strcmp(fault,"overlay") || !strcmp(fault,"foreign-filesystem") || !strcmp(fault,"foreign-overlay");
}
int __wrap_fstat(int fd,struct stat* st)
{
    int result=__real_fstat(fd,st);
    if(!result && device_fixture() && unit_descriptor(fd) && S_ISREG(st->st_mode)){st->st_dev^=1;}
    return result;
}
int __wrap_fstatfs(int fd,struct statfs* st)
{
    int result=__real_fstatfs(fd,st);
    if(!result && device_fixture() && unit_descriptor(fd)){
        st->f_type=!strcmp(fault,"foreign-filesystem")?0xef53:0x794c7630;
        memset(&st->f_fsid,0,sizeof(st->f_fsid));
        struct stat file;
        if(!strcmp(fault,"foreign-overlay") && !__real_fstat(fd,&file) && S_ISREG(file.st_mode)){
            memset(&st->f_fsid,1,sizeof(st->f_fsid));
        }
    }
    return result;
}
uint64_t pd_management_test_space(uint64_t actual){return !strcmp(fault,"no-space")?0:actual;}
int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
int pd_management_checkpoint(const char* point,unsigned index)
{
    char name[64];snprintf(name,sizeof(name),"%s:%u",point,index);
    if(!strncmp(fault,"kill:",5) && !strcmp(fault+5,name)){raise(SIGKILL);_exit(99);}
    return strncmp(fault,"fail:",5) || strcmp(fault+5,name);
}
static int read_file(int root,const char* name,unsigned char* data,size_t size)
{
    int fd=openat(root,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;
    int ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==(off_t)size && pread(fd,data,size,0)==(ssize_t)size;
    if(fd>=0){close(fd);}return ok;
}
int main(int argc,char** argv)
{
    if(argc!=3){return 2;}fault=argv[2];
    if(snprintf(unit_path,sizeof(unit_path),"%s/units",argv[1])>=(int)sizeof(unit_path)){return 2;}
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int store=openat(root,"store",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int units=openat(root,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct pd_upgrade approved;unsigned char record[PD_UPGRADE_APPROVAL_SIZE],previous_header[PD_RELEASE_PREFIX];
    struct pd_management_blob blob={0};struct pd_release candidate;
    int ok=root>=0 && read_file(root,"approved",record,sizeof(record)) && pd_upgrade_parse(record,sizeof(record),&approved) &&
        read_file(root,"previous.header",previous_header,sizeof(previous_header)) &&
        read_file(root,"candidate.header",blob.header,sizeof(blob.header)) &&
        read_file(root,"candidate.digest",blob.release_digest,32) &&
        pd_release_header_parse(previous_header,sizeof(previous_header),approved.previous_key,64,1,"4.3.16.42034",&approved.previous_release) &&
        pd_release_header_parse(blob.header,sizeof(blob.header),approved.candidate_key,64,1,"4.3.16.42034",&candidate);
    if(ok){
        blob.boot_size=candidate.length[0];blob.license_size=candidate.length[3];
        blob.boot=malloc(blob.boot_size);blob.licenses=malloc(blob.license_size);
        ok=blob.boot && blob.licenses && read_file(root,"boot",(unsigned char*)blob.boot,blob.boot_size) &&
            read_file(root,"licenses",(unsigned char*)blob.licenses,blob.license_size);
    }
    int lock=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(strcmp(fault,"no-lock")){ok=ok && lock>=0 && !flock(lock,LOCK_EX|LOCK_NB);}
    struct stat origin_stat;
    if(!fstatat(root,"origin",&origin_stat,AT_SYMLINK_NOFOLLOW)){
        struct pd_upgrade_origin origin;unsigned char bytes[PD_UPGRADE_ORIGIN_SIZE];
        ok=ok && read_file(root,"origin",bytes,sizeof(bytes)) &&
            pd_upgrade_origin_parse(bytes,sizeof(bytes),&approved,&origin) &&
            pd_management_apply_origin(store,units,&approved,&blob,&origin);
    }else{ok=ok && errno==ENOENT && pd_management_apply(store,units,&approved,&blob);}
    if(lock>=0){close(lock);}if(root>=0){close(root);}if(store>=0){close(store);}if(units>=0){close(units);}
    free((void*)blob.boot);free((void*)blob.licenses);return ok?0:1;
}
