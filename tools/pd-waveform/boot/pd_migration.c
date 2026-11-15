#define _GNU_SOURCE
#include "pd_migration.h"
#include "pd_state_transfer.h"
#include "pd_recovery.h"
#include "pd_readiness.h"
#include "pd_identity.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_MIGRATION_TEST
static uid_t migration_root(void){return getuid();}
extern int pd_migration_test_quiet(void);
static int migration_quiet(void){return pd_migration_test_quiet();}
extern int pd_migration_checkpoint(const char* point);
extern int pd_migration_test_readiness(const char*,struct pd_readiness*);
static int migration_readiness(const unsigned char* plan,struct pd_readiness* result){(void)plan;return pd_migration_test_readiness("",result);}
static int migration_ready_same(const unsigned char* plan,const struct pd_readiness* expected)
{
    struct pd_readiness observed;return migration_readiness(plan,&observed) && pd_readiness_equal(expected,&observed);
}
#else
static uid_t migration_root(void){return 0;}
static int migration_quiet(void){struct stat st;return lstat("/run/aetherpd",&st)<0 && errno==ENOENT;}
static int pd_migration_checkpoint(const char* point){(void)point;return 1;}
static int migration_lan(const unsigned char* plan,char address[16])
{
    struct pd_migration migration;struct pd_approval approved;struct pd_identity radio;
    if(!pd_migration_parse(plan,PD_MIGRATION_SIZE,&migration) ||
        !pd_approval_parse(migration.approval,PD_APPROVAL_SIZE,&approved) || !pd_identity_probe(&radio) ||
        !pd_identity_matches(&radio,approved.profile,approved.firmware,approved.serial) || !radio.address[0]){return 0;}
    /* Firmware's container endpoint is distinct from the LAN-only shim API.
     * The observed info reply on8600 firmware4.3.16 supplies the LAN address. */
    memcpy(address,radio.address,16);return 1;
}
static int migration_readiness(const unsigned char* plan,struct pd_readiness* result)
{
    char address[16];return migration_lan(plan,address) && pd_readiness_probe(address,result);
}
static int migration_ready_same(const unsigned char* plan,const struct pd_readiness* expected)
{
    char address[16];return migration_lan(plan,address) && pd_readiness_wait(address,expected);
}
#endif
static int migration_id(const unsigned char* p)
{
    int nonzero=0;for(unsigned i=0;i<64;++i){
        if(!((p[i]>='0' && p[i]<='9') || (p[i]>='a' && p[i]<='f'))){return 0;}
        nonzero|=p[i]!='0';
    }
    return nonzero;
}
int pd_migration_parse(const unsigned char* bytes,size_t size,struct pd_migration* result)
{
    memset(result,0,sizeof(*result));struct pd_approval context;
    if(size!=PD_MIGRATION_SIZE || memcmp(bytes,"PDMIG001",8) ||
        !pd_approval_parse(bytes+8,PD_APPROVAL_SIZE,&context) || context.profile!=1 ||
        !migration_id(bytes+148) || !migration_id(bytes+212)){return 0;}
    unsigned nonzero=0;for(unsigned i=276;i<308;++i){nonzero|=bytes[i];}if(!nonzero){return 0;}
    memcpy(result->approval,bytes+8,PD_APPROVAL_SIZE);
    memcpy(result->predecessor.container,bytes+148,64);memcpy(result->predecessor.image,bytes+212,64);
    memcpy(result->predecessor.executable,bytes+276,32);return 1;
}
static int migration_file(int fd,off_t size)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_nlink==1 &&
        st.st_uid==migration_root() && (st.st_mode&07777)==0600 && st.st_size==size;
}
static int migration_read_bytes(int fd,unsigned char* bytes,size_t size)
{
    size_t used=0;while(used<size){
        ssize_t n=pread(fd,bytes+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return 1;
}
static int migration_source(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && st.st_size>0 && st.st_size<=65536 && migration_file(fd,st.st_size);
}
int pd_migration_read(int fd,unsigned char bytes[PD_MIGRATION_SIZE],struct pd_migration* result)
{
    return migration_file(fd,PD_MIGRATION_SIZE) && migration_read_bytes(fd,bytes,PD_MIGRATION_SIZE) &&
        pd_migration_parse(bytes,PD_MIGRATION_SIZE,result);
}
int pd_migration_matches(const struct pd_migration* approved,const struct pd_predecessor* observed)
{
    return !strcmp(approved->predecessor.container,observed->container) &&
        !strcmp(approved->predecessor.image,observed->image) &&
        !crypto_verify32(approved->predecessor.executable,observed->executable);
}
static int migration_journal(int store,const unsigned char* record)
{
    int fd=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char prior[PD_MIGRATION_SIZE];
    if(fd>=0){int ok=migration_file(fd,PD_MIGRATION_SIZE) && migration_read_bytes(fd,prior,sizeof(prior)) &&
        !memcmp(prior,record,sizeof(prior));close(fd);return ok;}
    if(errno!=ENOENT){return 0;}
    fd=openat(store,"migration-approved.new",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd>=0){
        struct stat st;int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=PD_MIGRATION_SIZE &&
            migration_file(fd,st.st_size) && migration_read_bytes(fd,prior,(size_t)st.st_size) &&
            !memcmp(prior,record,(size_t)st.st_size);close(fd);
        if(!ok || unlinkat(store,"migration-approved.new",0)){return 0;}
    }else if(errno!=ENOENT){return 0;}
    fd=openat(store,"migration-approved.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t used=0;int ok=1;
    while(used<PD_MIGRATION_SIZE){
        ssize_t n=write(fd,record+used,PD_MIGRATION_SIZE-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && !renameat(store,"migration-approved.new",store,"migration-approved") && !fsync(store);
}
int pd_migration_bind(int store,const unsigned char record[PD_MIGRATION_SIZE])
{
    struct pd_migration parsed;struct stat st;
    if(!pd_migration_parse(record,PD_MIGRATION_SIZE,&parsed) || fstat(store,&st) ||
        !S_ISDIR(st.st_mode) || st.st_uid!=migration_root() || (st.st_mode&022)){return 0;}
    int lock=openat(store,"migration.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    int ok=migration_file(lock,0) && !flock(lock,LOCK_EX|LOCK_NB) && migration_journal(store,record);
    if(lock>=0){close(lock);}return ok;
}
static int migration_equal_files(int a,int b)
{
    struct stat left,right;
    if(a<0 || b<0 || fstat(a,&left) || fstat(b,&right) || !S_ISREG(left.st_mode) || !S_ISREG(right.st_mode) ||
        left.st_nlink!=1 || right.st_nlink!=1 || left.st_size<=0 || left.st_size>65536 || left.st_size!=right.st_size){return 0;}
    unsigned char x[1024],y[1024];off_t offset=0;int ok=1;
    while(ok && offset<left.st_size){
        size_t count=(size_t)(left.st_size-offset);if(count>sizeof(x)){count=sizeof(x);}
        ssize_t nx=pread(a,x,count,offset);if(nx<0 && errno==EINTR){continue;}
        ssize_t ny=pread(b,y,count,offset);if(ny<0 && errno==EINTR){continue;}
        if(nx!=(ssize_t)count || ny!=(ssize_t)count || memcmp(x,y,count)){ok=0;break;}offset+=(off_t)count;
    }
    crypto_wipe(x,sizeof(x));crypto_wipe(y,sizeof(y));return ok;
}
static int migration_current_copy(int store,int provision,int identity)
{
    int state=openat(store,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int ts=state>=0?openat(state,"tsnet",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int a=state>=0?openat(state,"provision.json",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC):-1;
    int b=ts>=0?openat(ts,"tailscaled.state",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC):-1;
    int ok=migration_equal_files(provision,a) && migration_equal_files(identity,b);
    if(a>=0){close(a);}if(b>=0){close(b);}if(ts>=0){close(ts);}if(state>=0){close(state);}return ok;
}
int pd_migration_copy(int store,const unsigned char record[PD_MIGRATION_SIZE])
{
    struct pd_migration approved;struct pd_predecessor observed;struct stat st;struct pd_readiness expected;
    if(!pd_migration_parse(record,PD_MIGRATION_SIZE,&approved) || !migration_quiet() ||
        fstat(store,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=migration_root() || (st.st_mode&022)){return 0;}
    int lock=openat(store,"migration.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(!migration_file(lock,0) || flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0){close(lock);}return 0;}
    int root=-1,state=-1,ts=-1,provision=-1,identity=-1;
    int ok=migration_quiet() && pd_predecessor_probe(&observed) && pd_migration_matches(&approved,&observed) &&
        pd_predecessor_image_verify(&observed) &&
        pd_predecessor_pin(&observed,&root,&state,&ts) && migration_readiness(record,&expected) &&
        migration_journal(store,record) && pd_readiness_prepare(store,record,&expected) &&
        pd_recovery_prepare(store,record,&observed) &&
        pd_migration_checkpoint("approved") && migration_quiet();
    int attempted=0;
    if(ok){attempted=1;ok=pd_predecessor_stop(&observed,root) && pd_migration_checkpoint("stopped");}
    if(ok){
        /* Pin directories before stop; open files after the writer exits so
         * its final atomic renames are included in the snapshot. */
        provision=openat(state,"provision.json",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        identity=openat(ts,"tailscaled.state",O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);
        ok=migration_source(provision) && migration_source(identity) && migration_quiet() &&
            pd_state_transfer(store,provision,identity,approved.approval) &&
            migration_current_copy(store,provision,identity) &&
            pd_predecessor_stopped(&observed) && migration_quiet() && pd_migration_checkpoint("copied");
    }
    int result=ok?1:0;
    if(!ok && attempted && (!migration_quiet() || !pd_predecessor_recover(&observed,root) ||
        !migration_ready_same(record,&expected))){result=-1;}
    if(provision>=0){close(provision);}if(identity>=0){close(identity);}
    if(ts>=0){close(ts);}if(state>=0){close(state);}if(root>=0){close(root);}close(lock);return result;
}
int pd_migration_recover(int store,const unsigned char record[PD_MIGRATION_SIZE])
{
    struct pd_migration approved;struct stat st;
    if(!pd_migration_parse(record,PD_MIGRATION_SIZE,&approved) || !migration_quiet() ||
        fstat(store,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=migration_root() || (st.st_mode&022)){return 0;}
    int lock=openat(store,"migration.lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(!migration_file(lock,0) || flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0){close(lock);}return 0;}
    int journal=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char prior[PD_MIGRATION_SIZE];struct pd_migration parsed;
    struct pd_recovery recipe;struct pd_readiness expected;
    int admitted=pd_migration_read(journal,prior,&parsed) && !memcmp(prior,record,sizeof(prior)) &&
        pd_recovery_read(store,record,&recipe) && pd_readiness_read(store,record,&expected) && migration_quiet();
    if(journal>=0){close(journal);}
    int result=0;
    if(admitted){
        result=pd_predecessor_recover_fresh(&approved.predecessor) &&
            migration_ready_same(record,&expected) && migration_quiet()?1:-1;
    }
    close(lock);return result;
}
