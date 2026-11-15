#define _GNU_SOURCE
#include "pd_upgrade_state.h"
#include "pd_transaction.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_TRANSACTION_TEST
static uid_t state_owner(void){return getuid();}
extern int pd_transaction_checkpoint(const char* point,unsigned phase);
#else
static uid_t state_owner(void){return 0;}
static int pd_transaction_checkpoint(const char* point,unsigned phase){(void)point;(void)phase;return 1;}
#endif
static int directory_ok(int fd)
{
    struct stat st;return !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==state_owner() && !(st.st_mode&07022);
}
static int file_ok(int fd,off_t size)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==state_owner() &&
        st.st_nlink==1 && (st.st_mode&07777)==0600 && st.st_size==size;
}
static int read_all(int fd,unsigned char* bytes,size_t size)
{
    size_t used=0;
    while(used<size){
        ssize_t n=pread(fd,bytes+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return 1;
}
static int terminal(unsigned phase){return phase==PD_UPGRADE_COMMITTED || phase==PD_UPGRADE_ROLLED_BACK;}
static int authority_named(int store,const char* name)
{
    int parent=openat(store,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(parent<0 || !directory_ok(parent)){if(parent>=0){close(parent);}return -1;}
    int result=fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW);
    int value=result<0?(errno==ENOENT?0:-1):
        S_ISDIR(st.st_mode) && st.st_uid==state_owner() && (st.st_mode&07777)==0700?1:-1;
    close(parent);return value;
}
static int authority(int store){return authority_named(store,"aetherpd-upgrade");}
static int installation_committed(int store)
{
    int fd=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char approval[PD_MIGRATION_SIZE];struct stat st;
    int ok=file_ok(fd,sizeof(approval)) && read_all(fd,approval,sizeof(approval)) &&
        pd_transaction_at_phase(store,approval,PD_TRANSACTION_COMMITTED) &&
        fstatat(store,"transaction.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        authority_named(store,"aetherpd-installation")==0;
    if(fd>=0){close(fd);}return ok;
}
static void encode(unsigned char bytes[PD_UPGRADE_STATE_SIZE],const unsigned char* approval,unsigned phase)
{
    memset(bytes,0,PD_UPGRADE_STATE_SIZE);memcpy(bytes,"PDUTXN01",8);
    memcpy(bytes+8,approval,PD_UPGRADE_APPROVAL_SIZE);bytes[PD_UPGRADE_STATE_SIZE-1]=(unsigned char)phase;
}
int pd_upgrade_state_parse(const unsigned char* bytes,size_t size,struct pd_upgrade_state* state)
{
    memset(state,0,sizeof(*state));unsigned char expected[PD_UPGRADE_STATE_SIZE];
    if(size!=PD_UPGRADE_STATE_SIZE || !pd_upgrade_parse(bytes+8,PD_UPGRADE_APPROVAL_SIZE,&state->approval)){return 0;}
    state->phase=bytes[size-1];encode(expected,state->approval.record,state->phase);
    return state->phase>=PD_UPGRADE_QUIESCING && state->phase<=PD_UPGRADE_ROLLED_BACK &&
        !memcmp(bytes,expected,sizeof(expected));
}
int pd_upgrade_state_read(int store,struct pd_upgrade_state* state)
{
    memset(state,0,sizeof(*state));if(!directory_ok(store)){return -1;}
    int fd=openat(store,"upgrade-transaction",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    unsigned char bytes[PD_UPGRADE_STATE_SIZE];
    int ok=file_ok(fd,sizeof(bytes)) && read_all(fd,bytes,sizeof(bytes));close(fd);
    return ok && pd_upgrade_state_parse(bytes,sizeof(bytes),state)?1:-1;
}
int pd_upgrade_admit(int store,int runtime_pending)
{
    struct pd_upgrade_state state;struct stat st;int present=pd_upgrade_state_read(store,&state);
    if(present<0 || !(fstatat(store,"upgrade-transaction.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT)){return 0;}
    int capsule=authority(store);
    if(runtime_pending){return capsule==1 && present==1 &&
        (state.phase==PD_UPGRADE_ACTIVATING || state.phase==PD_UPGRADE_RESTORING);}
    return capsule==0 && (present==0 || terminal(state.phase));
}
int pd_upgrade_at_phase(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],unsigned phase)
{
    struct pd_upgrade_state state;struct stat st;
    int lock=openat(store,"transaction.lock",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int ok=file_ok(lock,0) && flock(lock,LOCK_SH|LOCK_NB)<0 && errno==EWOULDBLOCK &&
        installation_committed(store) && pd_upgrade_state_read(store,&state)==1 && state.phase==phase &&
        !memcmp(state.approval.record,approval,PD_UPGRADE_APPROVAL_SIZE) && authority(store)==1 &&
        fstatat(store,"upgrade-transaction.new",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        flock(lock,LOCK_SH|LOCK_NB)<0 && errno==EWOULDBLOCK;
    if(lock>=0){close(lock);}return ok;
}
static int partial_matches(int store,const unsigned char* approval)
{
    int fd=openat(store,"upgrade-transaction.new",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    struct stat st;unsigned char bytes[PD_UPGRADE_STATE_SIZE],expected[PD_UPGRADE_STATE_SIZE];
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=PD_UPGRADE_STATE_SIZE &&
        file_ok(fd,st.st_size) && read_all(fd,bytes,(size_t)st.st_size);close(fd);
    if(!ok){return 0;}int matches=0;
    for(unsigned phase=PD_UPGRADE_QUIESCING;phase<=PD_UPGRADE_ROLLED_BACK;++phase){
        encode(expected,approval,phase);matches|=!memcmp(bytes,expected,(size_t)st.st_size);
    }
    return matches;
}
int pd_upgrade_worker_admit_predecessor(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                                       const struct pd_upgrade_state* predecessor)
{
    struct pd_upgrade parsed;struct pd_upgrade_state state;
    if(!pd_upgrade_parse(approval,PD_UPGRADE_APPROVAL_SIZE,&parsed) || !directory_ok(store) ||
        !installation_committed(store) || authority(store)!=1 || !partial_matches(store,approval)){return 0;}
    int present=pd_upgrade_state_read(store,&state);
    return (present==0 && !predecessor) || (present==1 &&
        (!memcmp(state.approval.record,approval,PD_UPGRADE_APPROVAL_SIZE) ||
         (predecessor && state.phase==predecessor->phase &&
          !memcmp(state.approval.record,predecessor->approval.record,PD_UPGRADE_APPROVAL_SIZE) &&
          pd_upgrade_follows(&state,&parsed))));
}
int pd_upgrade_worker_admit(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE])
{
    return pd_upgrade_worker_admit_predecessor(store,approval,NULL);
}
static int clear_partial(int store,const unsigned char* approval)
{
    return partial_matches(store,approval) &&
        (!unlinkat(store,"upgrade-transaction.new",0) || errno==ENOENT) && !fsync(store);
}
static int publish(int store,const unsigned char* approval,unsigned phase)
{
    if(!clear_partial(store,approval)){return 0;}
    unsigned char bytes[PD_UPGRADE_STATE_SIZE];encode(bytes,approval,phase);
    int fd=openat(store,"upgrade-transaction.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t used=0;int ok=1;
    while(used<sizeof(bytes)){
        ssize_t n=write(fd,bytes+used,sizeof(bytes)-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fsync(fd);if(close(fd)){ok=0;}
    return ok && pd_transaction_checkpoint("upgrade-file",phase) &&
        !renameat(store,"upgrade-transaction.new",store,"upgrade-transaction") &&
        pd_transaction_checkpoint("upgrade-rename",phase) && !fsync(store) &&
        pd_transaction_checkpoint("upgrade-durable",phase);
}
static int cleanup(int store,const struct pd_upgrade_ops* ops)
{
    return ops->cleanup(ops->context) && authority(store)==0;
}
static int rollback(int store,const unsigned char* approval,const struct pd_upgrade_ops* ops)
{
    if(!publish(store,approval,PD_UPGRADE_ROLLBACK) || !ops->rollback(ops->context) ||
        !publish(store,approval,PD_UPGRADE_RESTORING) || !ops->restore(ops->context) ||
        !publish(store,approval,PD_UPGRADE_ROLLED_BACK) || !cleanup(store,ops)){return -1;}
    return 0;
}
int pd_upgrade_follows(const struct pd_upgrade_state* prior,const struct pd_upgrade* next)
{
    const struct pd_approval* active=prior->phase==PD_UPGRADE_COMMITTED?&prior->approval.candidate:&prior->approval.previous;
    const unsigned char* key=prior->phase==PD_UPGRADE_COMMITTED?prior->approval.candidate_key:prior->approval.previous_key;
    return terminal(prior->phase) && active->profile==next->previous.profile &&
        !strcmp(active->firmware,next->previous.firmware) && !strcmp(active->serial,next->previous.serial) &&
        !memcmp(active->digest,next->previous.digest,32) && !memcmp(key,next->previous_key,32);
}
static int execute(int store,const unsigned char* approval,const struct pd_upgrade_ops* ops,unsigned phase,int recovery)
{
    if(terminal(phase)){
        if(!clear_partial(store,approval) || !pd_transaction_checkpoint("upgrade-terminal",phase) ||
            fsync(store) || !cleanup(store,ops)){return -1;}
        return phase==PD_UPGRADE_COMMITTED?1:0;
    }
    if(authority(store)!=1){return -2;}
    if(phase || recovery){return rollback(store,approval,ops);}
    const struct {unsigned phase;int (*action)(void*);} steps[]={
        {PD_UPGRADE_QUIESCING,ops->quiesce},{PD_UPGRADE_SWITCHING,ops->select},{PD_UPGRADE_ACTIVATING,ops->activate}};
    for(unsigned i=0;i<sizeof(steps)/sizeof(steps[0]);++i){
        if(!publish(store,approval,steps[i].phase)){return -1;}
        if(!steps[i].action(ops->context)){return rollback(store,approval,ops);}
    }
    if(!publish(store,approval,PD_UPGRADE_COMMITTED) || !cleanup(store,ops)){return -1;}
    return 1;
}
int pd_upgrade_transaction(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                            const struct pd_upgrade_ops* ops,int recovery)
{
    struct pd_upgrade approved;struct pd_upgrade_state state;
    if(!ops || !ops->rollback || !ops->restore || !ops->cleanup || (recovery!=0 && recovery!=1) ||
        (!recovery && (!ops->quiesce || !ops->select || !ops->activate)) ||
        !directory_ok(store) || !pd_upgrade_parse(approval,PD_UPGRADE_APPROVAL_SIZE,&approved)){return -2;}
    int lock=openat(store,"transaction.lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(!file_ok(lock,0) || flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0){close(lock);}return -2;}
    int present=installation_committed(store)?pd_upgrade_state_read(store,&state):-1,result=-2;
    if(present>=0){
        int same=present==0 || !memcmp(state.approval.record,approval,PD_UPGRADE_APPROVAL_SIZE);
        if(same || pd_upgrade_follows(&state,&approved)){
            unsigned phase=same?state.phase:0;struct stat partial;
            int pending=fstatat(store,"upgrade-transaction.new",&partial,AT_SYMLINK_NOFOLLOW);
            if(!pending || errno==ENOENT){
                if(!phase && !pending){phase=PD_UPGRADE_QUIESCING;}
                result=execute(store,approval,ops,phase,recovery);
            }
        }
    }
    close(lock);return result;
}
