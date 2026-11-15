#define _GNU_SOURCE
#include "pd_delivery.h"
#include "pd_bootstrap_artifact.h"
#include "pd_upgrade.h"
#include "pd_migration.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef PD_DELIVERY_TEST
static uid_t owner(void){return getuid();}
static const long kTimeoutMs=300;
extern void pd_delivery_test_exec(const char*,char* const*,char* const*);
#else
static uid_t owner(void){return 0;}
static const long kTimeoutMs=90000;
#endif
static const char* const kNames[]={"worker","plan","candidate","previous","approved","receipt"};
static volatile sig_atomic_t interrupted;
void pd_delivery_cancel(int signal){(void)signal;interrupted=1;}
struct file {int fd;struct stat identity;};
static uint32_t be32(const unsigned char* p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static int read_at(int fd,unsigned char* p,size_t n,off_t offset)
{
    while(n){ssize_t r=pread(fd,p,n,offset);if(r<0 && errno==EINTR){continue;}
        if(r<=0){return 0;}p+=r;n-=(size_t)r;offset+=r;}return 1;
}
static int record(int fd,unsigned char* p,size_t n,int protected)
{
    struct stat st;return !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size==(off_t)n &&
        (!protected || (st.st_uid==owner() && st.st_nlink<=1 && (st.st_mode&07777)==0600)) && read_at(fd,p,n,0);
}
static int directory(int fd)
{
    struct stat st;return !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==owner() && !(st.st_mode&0022);
}
static int same(const struct stat* a,const struct stat* b)
{
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino;
}
static int write_all(int fd,const unsigned char* p,size_t n)
{
    while(n){if(interrupted){return 0;}ssize_t w=write(fd,p,n);if(w<0 && errno==EINTR){continue;}
        if(w<=0){return 0;}p+=w;n-=(size_t)w;}return 1;
}
static int stage_file(int stage,unsigned index,const unsigned char* bytes,size_t length,int input,struct file* file)
{
    int fd=openat(stage,kNames[index],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}
    int ok=!fstat(fd,&file->identity);unsigned char buffer[16384];size_t offset=0;
    while(ok && offset<length){
        size_t size=length-offset;if(size>sizeof(buffer)){size=sizeof(buffer);}
        ok=(bytes || read_at(input,buffer,size,(off_t)offset)) && write_all(fd,bytes?bytes+offset:buffer,size);offset+=size;
    }
    ok=ok && !fchmod(fd,index==0?0700:0600) && !fsync(fd);close(fd);
    fd=ok?openat(stage,kNames[index],O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;
    if(fd>=0){file->fd=fcntl(fd,F_DUPFD_CLOEXEC,16);close(fd);}
    struct stat observed;
    return ok && file->fd>=0 && !fstat(file->fd,&observed) && same(&file->identity,&observed) && !fsync(stage);
}
static int release_size(int fd,size_t* size)
{
    struct stat st;int ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size>=PD_RELEASE_PREFIX && st.st_size<=PD_RELEASE_MAX;
    if(ok){*size=(size_t)st.st_size;}return ok;
}
static int target(const struct pd_approval* approved)
{
    struct pd_identity live;return pd_identity_probe(&live) &&
        pd_identity_matches(&live,approved->profile,approved->firmware,approved->serial);
}
static int cleanup(int run,int stage,const struct stat* identity,struct file files[6])
{
    int ok=1;struct stat st;
    for(unsigned i=0;i<6;++i){
        if(files[i].fd>=0){close(files[i].fd);files[i].fd=-1;}
        if(!files[i].identity.st_ino){continue;}
        if(fstatat(stage,kNames[i],&st,AT_SYMLINK_NOFOLLOW)){
            if(errno!=ENOENT){ok=0;}
        }else if(!same(&st,&files[i].identity) || !S_ISREG(st.st_mode) || st.st_uid!=owner() ||
                 st.st_nlink!=1 || unlinkat(stage,kNames[i],0)){ok=0;}
    }
    if(fsync(stage) || fstatat(run,"aetherpd-delivery",&st,AT_SYMLINK_NOFOLLOW) ||
       !same(&st,identity) || unlinkat(run,"aetherpd-delivery",AT_REMOVEDIR) || fsync(run)){ok=0;}
    close(stage);return ok;
}
/* A retry may discard only a prefix of the same authenticated inputs. The
 * protected receipt identifies the delivery; the directory lease excludes a
 * still-running supervisor or worker, including across parent death. */
static int recover_stage(int run,const unsigned char consent[80],const unsigned char* plan,size_t plan_size,
                         const struct pd_bootstrap_artifact* artifact,size_t candidate_size,size_t previous_size,uint32_t role)
{
    int stage=openat(run,"aetherpd-delivery",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    struct stat identity;unsigned char receipt[80];struct file files[6];memset(files,0,sizeof(files));
    for(unsigned i=0;i<6;++i){files[i].fd=-1;}
    if(stage<0){return 0;}
    int ok=directory(stage) && !fstat(stage,&identity) && (identity.st_mode&07777)==0700 &&
        !flock(stage,LOCK_EX|LOCK_NB);
    int marker=ok?openat(stage,"receipt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    ok=ok && marker>=0 && record(marker,receipt,sizeof(receipt),1) && !memcmp(receipt,consent,sizeof(receipt));
    if(marker>=0){close(marker);}
    DIR* entries=ok?fdopendir(dup(stage)):NULL;
    if(!entries){close(stage);return 0;}
    struct dirent* entry;errno=0;
    while(ok && (entry=readdir(entries))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        unsigned index=0;while(index<6 && strcmp(entry->d_name,kNames[index])){++index;}
        if(index==6){ok=0;break;}
        int fd=openat(stage,kNames[index],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        struct stat st;size_t limit=0;const unsigned char* bytes=NULL;int input=-1;
        if(index==0){limit=artifact->size;bytes=artifact->executable;}
        if(index==1){limit=plan_size;bytes=plan;}
        if(index==2){limit=candidate_size;input=5;}
        if(index==3 && role==PD_BOOTSTRAP_UPGRADE){limit=previous_size;input=6;}
        if(index==4 && role==PD_BOOTSTRAP_INSTALL){limit=PD_APPROVAL_SIZE;bytes=plan+8;}
        if(index==5){limit=80;bytes=consent;}
        ok=fd>=0 && !fstat(fd,&st) && limit && S_ISREG(st.st_mode) && st.st_uid==owner() &&
            st.st_nlink==1 && st.st_size>=0 && (uint64_t)st.st_size<=limit &&
            ((st.st_mode&07777)==0600 || (index==0 && (st.st_mode&07777)==0700));
        unsigned char actual[16384],expected[16384];
        for(off_t offset=0;ok && offset<st.st_size;){
            size_t size=(size_t)(st.st_size-offset);if(size>sizeof(actual)){size=sizeof(actual);}
            ok=read_at(fd,actual,size,offset) && (bytes || read_at(input,expected,size,offset)) &&
                !memcmp(actual,bytes?bytes+offset:expected,size);offset+=(off_t)size;
        }
        if(ok){files[index].identity=st;}
        if(fd>=0){close(fd);}errno=0;
    }
    if(errno){ok=0;}closedir(entries);
    if(!ok){close(stage);return 0;}
    return cleanup(run,stage,&identity,files);
}
static int close_other_fds(int keep)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return 0;}
    int listing=dirfd(directory);struct dirent* entry;
    while((entry=readdir(directory))){
        char* end=NULL;long fd=strtol(entry->d_name,&end,10);
        if(end==entry->d_name || *end || fd<6 || fd==keep || fd==7 || fd==listing){continue;}
        if(close((int)fd) && errno!=EBADF){closedir(directory);return 0;}
    }
    closedir(directory);return 1;
}
static int launch(uint32_t role,int stage,struct file files[6])
{
    if(interrupted){return 0;}
    struct timespec start,now;if(clock_gettime(CLOCK_MONOTONIC,&start)){return 0;}
    pid_t parent=getpid(),child=fork();if(child<0){return 0;}
    if(child==0){
        sigset_t mask;sigemptyset(&mask);
        if(sigprocmask(SIG_SETMASK,&mask,NULL)){_exit(120);}
        struct sigaction action={0};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
        if(sigaction(SIGTERM,&action,NULL) || sigaction(SIGINT,&action,NULL) || sigaction(SIGHUP,&action,NULL)){_exit(120);}
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent){_exit(120);}
        int ok=role==PD_BOOTSTRAP_UPGRADE?
            (dup2(files[1].fd,3)>=0 && dup2(files[3].fd,4)>=0 && dup2(files[2].fd,5)>=0):
            (dup2(files[2].fd,0)>=0 && dup2(files[4].fd,3)>=0 && dup2(files[1].fd,4)>=0);
        if(role==PD_BOOTSTRAP_UPGRADE){int null=open("/dev/null",O_RDONLY|O_CLOEXEC);ok=ok && null>=0 && dup2(null,0)>=0;if(null>=0){close(null);}}
        if(role==PD_BOOTSTRAP_INSTALL){close(5);}
        if(!ok || dup2(stage,7)<0 || fcntl(7,F_SETFD,0) || !close_other_fds(files[0].fd)){_exit(121);}
        char path[64];snprintf(path,sizeof(path),"/proc/self/fd/%d",files[0].fd);
        char* args[]={"aetherpd-worker","--delivered",NULL};char* env[]={"PATH=/usr/bin:/bin",NULL};
#ifdef PD_DELIVERY_TEST
        pd_delivery_test_exec(path,args,env);
#else
        execve(path,args,env);
        fprintf(stderr,"{\"delivery_exec_errno\":%d}\n",errno);
#endif
        _exit(122);
    }
    int status=0;
    for(;;){
        pid_t reaped=waitpid(child,&status,WNOHANG);
        if(reaped==child){
            int ok=WIFEXITED(status) && WEXITSTATUS(status)==0;
            if(!ok){fprintf(stderr,"{\"delivery_worker_exit\":%d,\"delivery_worker_signal\":%d}\n",
                WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);}
            return ok;
        }
        if(reaped<0 && errno!=EINTR){return 0;}
        if(interrupted || clock_gettime(CLOCK_MONOTONIC,&now) ||
           (now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000>=kTimeoutMs){
            kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR){}return 0;
        }
        struct timespec pause={0,10000000};nanosleep(&pause,NULL);
    }
}
int pd_delivery_run(int run,const unsigned char key[32],uint32_t arch)
{
    unsigned char consent[PD_DELIVERY_CONSENT_SIZE],plan[PD_UPGRADE_APPROVAL_SIZE],hash[32];
    if(interrupted || (arch!=7 && arch!=64) || !directory(run) || !record(3,consent,sizeof(consent),1) || memcmp(consent,"PDDELIV1",8) || be32(consent+8)!=arch){return 3;}
    uint32_t role=be32(consent+12);size_t plan_size=role==PD_BOOTSTRAP_INSTALL?PD_MIGRATION_SIZE:PD_UPGRADE_APPROVAL_SIZE;
    if((role!=PD_BOOTSTRAP_INSTALL && role!=PD_BOOTSTRAP_UPGRADE) ||
       (role==PD_BOOTSTRAP_INSTALL && arch!=64) || !record(4,plan,plan_size,0)){return 3;}
    crypto_blake2b(hash,32,plan,plan_size);if(crypto_verify32(hash,consent+48)){return 3;}
    struct pd_upgrade upgrade;struct pd_migration migration;struct pd_approval approved;
    if(role==PD_BOOTSTRAP_UPGRADE){
        if(!pd_upgrade_parse(plan,plan_size,&upgrade) || crypto_verify32(upgrade.candidate_key,key)){return 3;}
        approved=upgrade.candidate;
    }else if(!pd_migration_parse(plan,plan_size,&migration) ||
              !pd_approval_parse(migration.approval,PD_APPROVAL_SIZE,&approved)){return 3;}
    struct pd_release candidate,previous;struct pd_bootstrap_artifact artifact;size_t candidate_size=0,previous_size=0;
    if(!target(&approved) || !release_size(5,&candidate_size) ||
       !pd_release_read(5,key,arch,approved.profile,approved.firmware,approved.digest,&candidate) ||
       (role==PD_BOOTSTRAP_UPGRADE && (!release_size(6,&previous_size) || !pd_release_read(6,upgrade.previous_key,arch,
        upgrade.previous.profile,upgrade.previous.firmware,upgrade.previous.digest,&previous))) ||
       !pd_bootstrap_artifact_read(0,key,arch,role,candidate.manifest+16,approved.digest,&artifact)){return 3;}
    if(crypto_verify32(artifact.digest,consent+16)){pd_bootstrap_artifact_close(&artifact);return 3;}
    if(interrupted){pd_bootstrap_artifact_close(&artifact);return 3;}
    if(mkdirat(run,"aetherpd-delivery",0700)){
        if(errno!=EEXIST || !recover_stage(run,consent,plan,plan_size,&artifact,candidate_size,previous_size,role) ||
           interrupted || mkdirat(run,"aetherpd-delivery",0700)){pd_bootstrap_artifact_close(&artifact);return 3;}
    }
    int stage=openat(run,"aetherpd-delivery",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat identity;
    if(stage<0 || !directory(stage) || fstat(stage,&identity) || flock(stage,LOCK_EX|LOCK_NB)){
        if(stage>=0){close(stage);}pd_bootstrap_artifact_close(&artifact);return 5;
    }
    struct file files[6];memset(files,0,sizeof(files));for(unsigned i=0;i<6;++i){files[i].fd=-1;}
    int ok=stage_file(stage,5,consent,sizeof(consent),-1,&files[5]) &&
        stage_file(stage,0,artifact.executable,artifact.size,-1,&files[0]) &&
        stage_file(stage,1,plan,plan_size,-1,&files[1]) && stage_file(stage,2,NULL,candidate_size,5,&files[2]) &&
        (role!=PD_BOOTSTRAP_UPGRADE || stage_file(stage,3,NULL,previous_size,6,&files[3])) &&
        (role!=PD_BOOTSTRAP_INSTALL || stage_file(stage,4,migration.approval,PD_APPROVAL_SIZE,-1,&files[4]));
    pd_bootstrap_artifact_close(&artifact);
    ok=ok && pd_release_read(files[2].fd,key,arch,approved.profile,approved.firmware,approved.digest,&candidate) &&
        (role!=PD_BOOTSTRAP_UPGRADE || pd_release_read(files[3].fd,upgrade.previous_key,arch,
         upgrade.previous.profile,upgrade.previous.firmware,upgrade.previous.digest,&previous)) && target(&approved);
    int result=ok?(launch(role,stage,files)?0:4):3;
    return cleanup(run,stage,&identity,files)?result:5;
}
