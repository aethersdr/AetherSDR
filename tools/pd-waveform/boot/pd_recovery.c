#define _GNU_SOURCE
#include "pd_recovery.h"
#include "monocypher.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(PD_MIGRATION_TEST) || defined(PD_RECOVERY_TEST)
static uid_t recovery_root(void){return getuid();}
#else
static uid_t recovery_root(void){return 0;}
#endif
#ifdef PD_MIGRATION_TEST
extern int pd_migration_checkpoint(const char* point);
static int recovery_checkpoint(const char* point){return pd_migration_checkpoint(point);}
#else
static int recovery_checkpoint(const char* point){(void)point;return 1;}
#endif
static int recovery_address(const char address[16])
{
    const char* end=memchr(address,0,16);if(!end || end==address){return 0;}
    for(const char* p=end;p<address+16;++p){if(*p){return 0;}}
    struct in_addr parsed;char canonical[INET_ADDRSTRLEN];
    return inet_pton(AF_INET,address,&parsed)==1 && inet_ntop(AF_INET,&parsed,canonical,sizeof(canonical)) &&
        !strcmp(address,canonical);
}
int pd_recovery_parse(const unsigned char* bytes,size_t size,struct pd_recovery* result)
{
    memset(result,0,sizeof(*result));struct pd_recovery parsed={0};
    if(size!=PD_RECOVERY_SIZE || memcmp(bytes,"PDREC001",8) ||
        !pd_migration_parse(bytes+8,PD_MIGRATION_SIZE,&parsed.migration)){return 0;}
    memcpy(parsed.radio_address,bytes+316,16);if(!recovery_address(parsed.radio_address)){return 0;}
    memcpy(parsed.migration.predecessor.radio_address,parsed.radio_address,16);
    unsigned char digest[32];crypto_blake2b(digest,32,bytes,size);static const char hex[]="0123456789abcdef";
    memcpy(parsed.hostname,"aetherpd-",9);
    for(unsigned i=0;i<16;++i){parsed.hostname[9+2*i]=hex[digest[i]>>4];parsed.hostname[10+2*i]=hex[digest[i]&15];}
    *result=parsed;return 1;
}
static int recovery_store(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==recovery_root() && !(st.st_mode&022);
}
static int recovery_file(int fd,off_t* size)
{
    struct stat st;int ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==recovery_root() &&
        st.st_nlink==1 && (st.st_mode&07777)==0600 && st.st_size>=0 && st.st_size<=PD_RECOVERY_SIZE;
    if(ok){*size=st.st_size;}return ok;
}
static int recovery_bytes(int fd,unsigned char* bytes,size_t size)
{
    size_t used=0;while(used<size){
        ssize_t n=pread(fd,bytes+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return 1;
}
int pd_recovery_read(int store,const unsigned char record[PD_MIGRATION_SIZE],struct pd_recovery* result)
{
    memset(result,0,sizeof(*result));if(!recovery_store(store)){return 0;}
    int fd=openat(store,"recovery-recipe",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);off_t size=0;
    unsigned char bytes[PD_RECOVERY_SIZE];
    int ok=recovery_file(fd,&size) && size==PD_RECOVERY_SIZE && recovery_bytes(fd,bytes,sizeof(bytes)) &&
        !memcmp(bytes+8,record,PD_MIGRATION_SIZE) && pd_recovery_parse(bytes,sizeof(bytes),result);
    if(fd>=0){close(fd);}return ok;
}
int pd_recovery_prepare(int store,const unsigned char record[PD_MIGRATION_SIZE],const struct pd_predecessor* observed)
{
    struct pd_migration approved;struct pd_recovery parsed;
    if(!recovery_store(store) || !pd_migration_parse(record,PD_MIGRATION_SIZE,&approved) ||
        !pd_migration_matches(&approved,observed) || !recovery_address(observed->radio_address)){return 0;}
    unsigned char bytes[PD_RECOVERY_SIZE],prior[PD_RECOVERY_SIZE];
    memcpy(bytes,"PDREC001",8);memcpy(bytes+8,record,PD_MIGRATION_SIZE);memcpy(bytes+316,observed->radio_address,16);
    int fd=openat(store,"recovery-recipe",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);off_t size=0;
    if(fd>=0){
        int ok=recovery_file(fd,&size) && size==PD_RECOVERY_SIZE && recovery_bytes(fd,prior,sizeof(prior)) &&
            !memcmp(prior,bytes,sizeof(prior));close(fd);return ok && !fsync(store);
    }
    if(errno!=ENOENT || !pd_recovery_parse(bytes,sizeof(bytes),&parsed)){return 0;}
    fd=openat(store,"recovery-recipe.new",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0){
        int ok=recovery_file(fd,&size) && recovery_bytes(fd,prior,(size_t)size) && !memcmp(prior,bytes,(size_t)size);
        close(fd);if(!ok || unlinkat(store,"recovery-recipe.new",0)){return 0;}
    }else if(errno!=ENOENT){return 0;}
    fd=openat(store,"recovery-recipe.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}int ok=1;size_t used=0;
    while(used<sizeof(bytes)){
        ssize_t n=write(fd,bytes+used,sizeof(bytes)-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && recovery_checkpoint("recipe-file") &&
        !renameat(store,"recovery-recipe.new",store,"recovery-recipe") &&
        recovery_checkpoint("recipe-rename") && !fsync(store);
}
