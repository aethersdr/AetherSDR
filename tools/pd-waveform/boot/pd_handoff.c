#define _GNU_SOURCE
#include "pd_handoff.h"
#include "pd_bundle.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static uid_t handoff_owner(void)
{
#ifdef PD_UPGRADE_TEST
    return getuid();
#else
    return 0;
#endif
}
static int handoff_close_others(void)
{
    DIR* directory=opendir("/proc/self/fd");if(!directory){return 0;}
    int own=dirfd(directory),ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(directory);if(!entry){ok=!errno;break;}
        if(entry->d_name[0]=='.'){continue;}
        char* end=NULL;errno=0;long fd=strtol(entry->d_name,&end,10);
        if(errno || !end || *end || fd<0 || fd>INT_MAX){ok=0;break;}
        if(fd>2 && fd!=own && fd!=PD_HANDOFF_LEASE && fd!=PD_HANDOFF_PAYLOAD && fd!=PD_HANDOFF_BOOT){close((int)fd);}
    }
    closedir(directory);return ok;
}
int pd_handoff_exec(const struct pd_pair* pair,int socket_owner)
{
    if(getuid()!=handoff_owner() || geteuid()!=handoff_owner() || pair->choice>PD_PAIR_CANDIDATE ||
        pair->boot_handoff>1 || (socket_owner!=0 && socket_owner!=1)){return 0;}
    int original[3]={pair->lease,pair->payload,pair->boot},copies[3]={-1,-1,-1};int ok=1;
    for(unsigned i=0;ok && i<3;++i){copies[i]=fcntl(original[i],F_DUPFD_CLOEXEC,128);ok=copies[i]>=0;}
    for(unsigned i=0;ok && i<3;++i){ok=dup2(copies[i],PD_HANDOFF_LEASE+(int)i)>=0;}
    for(unsigned i=0;i<3;++i){if(copies[i]>=0){close(copies[i]);}}
    ok=ok && !chdir("/") && handoff_close_others();
    if(ok){
        char* args[]={"aetherpd-paired-boot",socket_owner?"--socket-owner":NULL,NULL};
        char* env[]={"PATH=/usr/bin:/bin",pair->boot_handoff?"AETHERPD_PAIRED_BOOT=1":NULL,NULL};
        fexecve(PD_HANDOFF_BOOT,args,env);
    }
    for(int fd=PD_HANDOFF_LEASE;fd<=PD_HANDOFF_BOOT;++fd){close(fd);}return 0;
}
static int handoff_file(int fd,dev_t device,mode_t mode,int access,struct stat* st)
{
    int flags=fcntl(fd,F_GETFL);
    return flags>=0 && (flags&O_ACCMODE)==access && !fstat(fd,st) && S_ISREG(st->st_mode) &&
        st->st_uid==handoff_owner() && st->st_nlink==1 && (st->st_mode&07777)==mode && st->st_dev==device;
}
int pd_handoff_receive(int control,const unsigned char key[32],uint32_t arch)
{
    const char* marker=getenv("AETHERPD_PAIRED_BOOT");struct stat directory,lease,actual,payload,boot,self;
    if(getuid()!=handoff_owner() || geteuid()!=handoff_owner() || !marker || strcmp(marker,"1") ||
        fstat(control,&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid!=handoff_owner() ||
        (directory.st_mode&07777)!=0700 ||
        !handoff_file(PD_HANDOFF_LEASE,directory.st_dev,0600,O_RDWR,&lease) || lease.st_size ||
        fstatat(control,"lock",&actual,AT_SYMLINK_NOFOLLOW) || actual.st_dev!=lease.st_dev || actual.st_ino!=lease.st_ino ||
        !handoff_file(PD_HANDOFF_PAYLOAD,directory.st_dev,0644,O_RDONLY,&payload) ||
        !handoff_file(PD_HANDOFF_BOOT,directory.st_dev,0755,O_RDONLY,&boot)){return -1;}
    int executable=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
    int ok=executable>=0 && !fstat(executable,&self) && self.st_dev==boot.st_dev && self.st_ino==boot.st_ino;
    if(executable>=0){close(executable);}
    struct pd_bundle bundle;
    ok=ok && !flock(PD_HANDOFF_LEASE,LOCK_SH|LOCK_NB) &&
        pd_bundle_read(PD_HANDOFF_PAYLOAD,key,arch,&bundle) && pd_bundle_validate(PD_HANDOFF_PAYLOAD,&bundle);
    int result=ok?fcntl(PD_HANDOFF_PAYLOAD,F_DUPFD_CLOEXEC,3):-1;
    if(result>=0 && fcntl(PD_HANDOFF_LEASE,F_SETFD,FD_CLOEXEC)){close(result);result=-1;}
    close(PD_HANDOFF_PAYLOAD);close(PD_HANDOFF_BOOT);return result;
}
