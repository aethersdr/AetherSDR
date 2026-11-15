#define _GNU_SOURCE
#include "pd_installation.h"
#include "pd_generation.h"
#include "pd_pair.h"
#include "pd_transaction.h"
#include "monocypher.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#ifdef PD_INSTALLATION_TEST
static uid_t owner(void){return getuid();}
extern void pd_installation_checkpoint(unsigned point);
#else
static uid_t owner(void){return 0;}
static void pd_installation_checkpoint(unsigned point){(void)point;}
#endif
static int protected_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==owner() && !(st.st_mode&07022);
}
static int read_record(int directory,const char* name,unsigned char* bytes,size_t size,mode_t mode,int partial)
{
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    struct stat st;int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==owner() && st.st_nlink==1 &&
        st.st_size>=0 && st.st_size<=(off_t)size && (partial || st.st_size==(off_t)size) &&
        ((st.st_mode&07777)==mode || (partial && (st.st_mode&07777)==0600));
    size_t used=0,length=ok?(size_t)st.st_size:0;
    while(ok && used<length){
        ssize_t n=pread(fd,bytes+used,length-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    close(fd);return ok?(int)length+1:-1;
}
static int canonical(const unsigned char bytes[PD_INSTALLATION_SIZE])
{
    unsigned phase=bytes[11],profile=bytes[15];
    return !memcmp(bytes,"PDVIEW01",8) && !bytes[8] && !bytes[9] && !bytes[10] &&
        (phase==PD_UPGRADE_COMMITTED || phase==PD_UPGRADE_ROLLED_BACK) &&
        !bytes[12] && !bytes[13] && !bytes[14] && (profile==1 || profile==2) &&
        memchr(bytes+112,0,28) && memchr(bytes+140,0,64);
}
static int publish(int runtime,const unsigned char bytes[PD_INSTALLATION_SIZE])
{
    unsigned char previous[PD_INSTALLATION_SIZE];
    int present=read_record(runtime,"installation",previous,sizeof(previous),0444,0);
    if(present<0 || (present && !canonical(previous))){return 0;}
    int partial=read_record(runtime,".installation.new",previous,sizeof(previous),0444,1);
    if(partial<0 || (partial && memcmp(previous,bytes,(size_t)partial-1))){return 0;}
    if(partial && unlinkat(runtime,".installation.new",0)){return 0;}
    int fd=openat(runtime,".installation.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}pd_installation_checkpoint(1);
    int ok=1;size_t used=0;
    while(used<PD_INSTALLATION_SIZE){
        ssize_t n=write(fd,bytes+used,PD_INSTALLATION_SIZE-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    pd_installation_checkpoint(2);
    ok=ok && !fchmod(fd,0444) && !fsync(fd);if(close(fd)){ok=0;}
    pd_installation_checkpoint(3);
    ok=ok && !renameat(runtime,".installation.new",runtime,"installation");
    pd_installation_checkpoint(4);
    return ok && !fsync(runtime);
}
int pd_installation_publish(int store,int runtime)
{
    struct statfs fs;
    if(!protected_directory(runtime) || fstatfs(runtime,&fs) || (unsigned long)fs.f_type!=0x01021994UL){return 0;}
    int lease=pd_transaction_session_lock(store);
    if(lease<0){return 0;}
    struct pd_upgrade_state state;struct pd_generation generation={.directory=-1,.stage=-1,.control=-1};
    int locked=!flock(runtime,LOCK_EX|LOCK_NB);
    int ok=locked && pd_upgrade_state_read(store,&state)==1 &&
        (state.phase==PD_UPGRADE_COMMITTED || state.phase==PD_UPGRADE_ROLLED_BACK) && pd_generation_active(store,&generation)==1;
    unsigned char bytes[PD_INSTALLATION_SIZE]={0},selected[PD_PAIR_RECORD_SIZE],expected[PD_PAIR_RECORD_SIZE]={0};
    if(ok){
        unsigned choice=state.phase==PD_UPGRADE_COMMITTED?PD_PAIR_CANDIDATE:PD_PAIR_PREVIOUS;
        const struct pd_approval* target=choice==PD_PAIR_CANDIDATE?&state.approval.candidate:&state.approval.previous;
        const unsigned char* key=choice==PD_PAIR_CANDIDATE?state.approval.candidate_key:state.approval.previous_key;
        memcpy(bytes,"PDVIEW01",8);bytes[11]=(unsigned char)state.phase;bytes[15]=(unsigned char)target->profile;
        crypto_blake2b(bytes+16,32,state.approval.record,PD_UPGRADE_APPROVAL_SIZE);
        memcpy(bytes+48,target->digest,32);memcpy(bytes+80,key,32);
        memcpy(bytes+112,target->firmware,strlen(target->firmware));memcpy(bytes+140,target->serial,strlen(target->serial));
        memcpy(expected,"PDPAIR01",8);expected[8]=(unsigned char)choice;memcpy(expected+16,bytes+16,32);memcpy(expected+48,bytes+48,32);
        ok=read_record(generation.control,"active",selected,sizeof(selected),0600,0)==PD_PAIR_RECORD_SIZE+1 &&
            !memcmp(selected,expected,sizeof(selected)) && publish(runtime,bytes);
    }
    pd_generation_close(&generation);if(locked){flock(runtime,LOCK_UN);}close(lease);return ok;
}
int pd_installation_publish_runtime(int store)
{
    int fd=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    const char* const parts[]={"run","aetherpd"};
    for(unsigned i=0;fd>=0 && i<2;++i){
        int next=protected_directory(fd)?openat(fd,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
        close(fd);fd=next;
    }
    int ok=fd>=0 && pd_installation_publish(store,fd);if(fd>=0){close(fd);}return ok;
}
