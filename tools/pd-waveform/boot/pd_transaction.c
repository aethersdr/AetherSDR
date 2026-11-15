#define _GNU_SOURCE
#include "pd_transaction.h"
#include "pd_upgrade_state.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum { kTransactionSize=8+PD_MIGRATION_SIZE+4 };
#ifdef PD_TRANSACTION_TEST
static uid_t transaction_owner(void){return getuid();}
extern int pd_transaction_checkpoint(const char* point,unsigned phase);
#else
static uid_t transaction_owner(void){return 0;}
static int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
#endif
static int transaction_file(int fd,off_t size)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) &&
        st.st_uid==transaction_owner() && st.st_nlink==1 && (st.st_mode&07777)==0600 && st.st_size==size;
}
static void transaction_encode(unsigned char bytes[kTransactionSize],const unsigned char* approval,unsigned phase)
{
    memcpy(bytes,"PDTXN001",8);memcpy(bytes+8,approval,PD_MIGRATION_SIZE);
    bytes[kTransactionSize-4]=bytes[kTransactionSize-3]=bytes[kTransactionSize-2]=0;
    bytes[kTransactionSize-1]=(unsigned char)phase;
}
static int transaction_read(int fd,unsigned char* bytes,size_t size)
{
    size_t used=0;
    while(used<size){
        ssize_t n=pread(fd,bytes+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return 1;
}
static int transaction_phase(int store,const unsigned char* approval,unsigned* phase)
{
    *phase=0;int fd=openat(store,"transaction",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    unsigned char bytes[kTransactionSize],expected[kTransactionSize];
    int ok=transaction_file(fd,kTransactionSize) && transaction_read(fd,bytes,sizeof(bytes));close(fd);
    if(!ok){return 0;}unsigned value=bytes[kTransactionSize-1];transaction_encode(expected,approval,value);
    if(value<PD_TRANSACTION_PREPARING || value>PD_TRANSACTION_ROLLED_BACK || memcmp(bytes,expected,sizeof(bytes))){return 0;}
    *phase=value;return 1;
}
int pd_transaction_at_phase(int store,const unsigned char approval[PD_MIGRATION_SIZE],unsigned phase)
{
    struct stat st;struct pd_migration parsed;unsigned observed=0;
    return phase>=PD_TRANSACTION_PREPARING && phase<=PD_TRANSACTION_ROLLED_BACK &&
        pd_migration_parse(approval,PD_MIGRATION_SIZE,&parsed) && !fstat(store,&st) && S_ISDIR(st.st_mode) &&
        st.st_uid==transaction_owner() && !(st.st_mode&022) && transaction_phase(store,approval,&observed) && observed==phase;
}
static int transaction_gate_phase(int store,unsigned* phase)
{
    int approved=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char record[PD_MIGRATION_SIZE];struct pd_migration parsed;struct stat st;
    int ok=transaction_file(approved,sizeof(record)) && transaction_read(approved,record,sizeof(record)) &&
        pd_migration_parse(record,sizeof(record),&parsed) && transaction_phase(store,record,phase);
    if(approved>=0){close(approved);}
    return ok && fstatat(store,"transaction.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
}
static int transaction_authority_absent(int store)
{
    int parent=openat(store,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    int ok=parent>=0 && !fstat(parent,&st) && S_ISDIR(st.st_mode) && st.st_uid==transaction_owner() && !(st.st_mode&022) &&
        fstatat(parent,"aetherpd-installation",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    if(parent>=0){close(parent);}return ok;
}
static int transaction_gate(int store,int runtime,int* lease)
{
    *lease=-1;struct stat st;unsigned phase=0;
    if(fstat(store,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=transaction_owner() || (st.st_mode&022)){return 0;}
    int lock=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(!transaction_file(lock,0)){if(lock>=0){close(lock);}return 0;}
    if(flock(lock,LOCK_SH|LOCK_NB)){
        int activating=runtime && errno==EWOULDBLOCK && transaction_gate_phase(store,&phase) &&
            ((phase==PD_TRANSACTION_ACTIVATING && pd_upgrade_admit(store,0)) ||
             (phase==PD_TRANSACTION_COMMITTED && transaction_authority_absent(store) && pd_upgrade_admit(store,1)));
        /* A stale ACTIVATING record without its worker cannot boot a new
         * identity owner. Recheck the exclusive owner after reading intent. */
        activating=activating && flock(lock,LOCK_SH|LOCK_NB)<0 && errno==EWOULDBLOCK;
        close(lock);return activating;
    }
    /* An interrupted publication is reconciled by the installer, never here.
     * Sync the directory before admitting a commit observed after worker death. */
    int ok=transaction_gate_phase(store,&phase) && phase==PD_TRANSACTION_COMMITTED &&
        transaction_authority_absent(store) && pd_upgrade_admit(store,0) && !fsync(store);
    if(!ok){close(lock);return 0;}*lease=lock;return 1;
}
int pd_transaction_session_lock(int store)
{
    int lease=-1;return transaction_gate(store,0,&lease)?lease:-1;
}
int pd_transaction_runtime_gate(int store,int* lease)
{
    if(!lease){return 0;}return transaction_gate(store,1,lease);
}
static int transaction_store(void)
{
    if(getuid()!=0 || geteuid()!=0){return -1;}
#if defined(__aarch64__)
    const char* const parts[]={"var","lib","smartsdr","aetherpd"};
#elif defined(__arm__)
    const char* const parts[]={"user","aetherpd"};
#else
    return -1;
    const char* const parts[]={"user","aetherpd"};
#endif
    int directory=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(directory<0 || fstat(directory,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&022)){
        if(directory>=0){close(directory);}return -1;
    }
    for(unsigned i=0;i<sizeof(parts)/sizeof(parts[0]);++i){
        int next=openat(directory,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(directory);directory=next;
        if(directory<0 || fstat(directory,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=0 || (st.st_mode&022)){
            if(directory>=0){close(directory);}return -1;
        }
    }
    return directory;
}
int pd_transaction_session_open(void)
{
    int store=transaction_store();if(store<0){return -1;}
    int lock=pd_transaction_session_lock(store);close(store);return lock;
}
int pd_transaction_runtime_open(int* lease)
{
    if(!lease){return 0;}*lease=-1;int store=transaction_store();if(store<0){return 0;}
    int ok=pd_transaction_runtime_gate(store,lease);close(store);return ok;
}
static int transaction_clear_partial(int store,const unsigned char* approval)
{
    int fd=openat(store,"transaction.new",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    struct stat st;unsigned char bytes[kTransactionSize],expected[kTransactionSize];
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=kTransactionSize &&
        transaction_file(fd,st.st_size) && transaction_read(fd,bytes,(size_t)st.st_size);close(fd);
    if(!ok){return 0;}int matches=0;
    for(unsigned phase=PD_TRANSACTION_PREPARING;phase<=PD_TRANSACTION_ROLLED_BACK;++phase){
        transaction_encode(expected,approval,phase);matches|=!memcmp(bytes,expected,(size_t)st.st_size);
    }
    return matches && !unlinkat(store,"transaction.new",0) && !fsync(store);
}
static int transaction_write(int store,const unsigned char* approval,unsigned phase)
{
    if(!transaction_clear_partial(store,approval)){return 0;}
    unsigned char bytes[kTransactionSize];transaction_encode(bytes,approval,phase);
    int fd=openat(store,"transaction.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t used=0;int ok=1;
    while(used<sizeof(bytes)){
        ssize_t n=write(fd,bytes+used,sizeof(bytes)-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && pd_transaction_checkpoint("file",phase) &&
        !renameat(store,"transaction.new",store,"transaction") &&
        pd_transaction_checkpoint("rename",phase) && !fsync(store) &&
        pd_transaction_checkpoint("durable",phase);
}
static int transaction_rollback(int store,const unsigned char* approval,const struct pd_transaction_ops* ops)
{
    if(!transaction_write(store,approval,PD_TRANSACTION_ROLLBACK) || !ops->rollback(ops->context) ||
        !transaction_write(store,approval,PD_TRANSACTION_ROLLED_BACK) || !ops->cleanup(ops->context)){return -1;}
    return 0;
}
static int transaction_execute(int store,const unsigned char* approval,const struct pd_transaction_ops* ops,unsigned phase,int recovery)
{
    if(phase==PD_TRANSACTION_COMMITTED || phase==PD_TRANSACTION_ROLLED_BACK){
        /* Re-entry can observe a rename whose directory sync was interrupted. */
        if(!transaction_clear_partial(store,approval) || !pd_transaction_checkpoint("terminal",phase) ||
            fsync(store) || !ops->cleanup(ops->context)){return -1;}
        return phase==PD_TRANSACTION_COMMITTED?1:0;
    }
    if(phase || recovery){return transaction_rollback(store,approval,ops);}
    const struct {unsigned phase;int (*action)(void*);} steps[]={
        {PD_TRANSACTION_PREPARING,ops->prepare},{PD_TRANSACTION_COPYING,ops->copy},
        {PD_TRANSACTION_RETIRING,ops->retire},{PD_TRANSACTION_ACTIVATING,ops->activate}};
    for(unsigned i=0;i<sizeof(steps)/sizeof(steps[0]);++i){
        if(!transaction_write(store,approval,steps[i].phase)){return -1;}
        if(!steps[i].action(ops->context)){return transaction_rollback(store,approval,ops);}
    }
    if(!transaction_write(store,approval,PD_TRANSACTION_COMMITTED) || !ops->cleanup(ops->context)){return -1;}
    return 1;
}
static int transaction_run(int store,const unsigned char* approval,const struct pd_transaction_ops* ops,int recovery)
{
    struct stat st;struct pd_migration parsed;
    if(!ops || !ops->rollback || !ops->cleanup ||
        (!recovery && (!ops->prepare || !ops->copy || !ops->retire || !ops->activate)) ||
        !pd_migration_parse(approval,PD_MIGRATION_SIZE,&parsed) || fstat(store,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=transaction_owner() || (st.st_mode&022)){return -2;}
    int lock=openat(store,"transaction.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(!transaction_file(lock,0) || flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0){close(lock);}return -2;}
    unsigned phase=0;int result=transaction_phase(store,approval,&phase)?transaction_execute(store,approval,ops,phase,recovery):-2;
    close(lock);return result;
}
int pd_transaction_run(int store,const unsigned char approval[PD_MIGRATION_SIZE],const struct pd_transaction_ops* ops)
{
    return transaction_run(store,approval,ops,0);
}
int pd_transaction_recover(int store,const unsigned char approval[PD_MIGRATION_SIZE],const struct pd_transaction_ops* ops)
{
    return transaction_run(store,approval,ops,1);
}
