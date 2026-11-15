#define main inherited_shim_main
#include "../session/pd_shim_launch.c"
#undef main
#include "pd_release.h"
#include "pd_release_key.h"
#include "pd_approval.h"

/* The authenticated bootstrap supplies the approved artifact and target on fd3.
 * Staging authorizes no installation: live hardware and migration admission
 * must still pass before any persistent mutation or payload execution. */
static int stage_remove(int parent,int stage)
{
    int ok=1;
    for(unsigned i=0;i<PD_RELEASE_COUNT;++i){
        if(unlinkat(stage,pd_release_names[i],0) && errno!=ENOENT){ok=0;}
    }
    if(unlinkat(stage,"approved",0) && errno!=ENOENT){ok=0;}
    return ok && !unlinkat(parent,"aetherpd-install.stage",AT_REMOVEDIR);
}
#ifdef PD_WORKER_TEST
extern int pd_stage_checkpoint(const char* point);
#else
static int pd_stage_checkpoint(const char* point){(void)point;return 1;}
#endif
static int stage_partial(int stage,const struct pd_release* release,const unsigned char approved[PD_APPROVAL_SIZE])
{
    struct stat st;
    if(fstat(stage,&st) || !protected_directory(&st) || (st.st_mode&07777)!=0700){return 0;}
    int copy=openat(stage,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(listing);if(!entry){ok=!errno;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        unsigned i=0;for(;i<PD_RELEASE_COUNT && strcmp(entry->d_name,pd_release_names[i]);++i){}
        if(i==PD_RELEASE_COUNT && strcmp(entry->d_name,"approved")){ok=0;break;}
        int fd=openat(stage,entry->d_name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        off_t limit=i<PD_RELEASE_COUNT?release->length[i]:PD_APPROVAL_SIZE;
        mode_t mode=i<2?0755:i<PD_RELEASE_COUNT?0644:0600;
        ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 &&
            st.st_size>=0 && st.st_size<=limit && ((st.st_mode&07777)==0600 || (st.st_mode&07777)==mode);
        unsigned char actual[16384],expected[16384];off_t offset=0;
        while(ok && offset<st.st_size){
            size_t count=(size_t)(st.st_size-offset);if(count>sizeof(actual)){count=sizeof(actual);}
            ssize_t n=pread(fd,actual,count,offset);if(n<0 && errno==EINTR){continue;}
            if(n!=(ssize_t)count){ok=0;break;}
            if(i<PD_RELEASE_COUNT){
                n=pread(0,expected,count,(off_t)release->offset[i]+offset);
                if(n<0 && errno==EINTR){continue;}if(n!=(ssize_t)count){ok=0;break;}
            }else{memcpy(expected,approved+(size_t)offset,count);}
            ok=!memcmp(actual,expected,count);offset+=(off_t)count;
        }
        if(fd>=0){close(fd);}if(!ok){break;}
    }
    closedir(listing);return ok;
}
#ifndef PD_STAGE_MAIN
#define PD_STAGE_MAIN main
#endif
int PD_STAGE_MAIN(int argc,char** argv)
{
    (void)argv;
    if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
#if defined(__aarch64__) || defined(PD_WORKER_TEST)
    const uint32_t arch=64;
#elif defined(__arm__)
    const uint32_t arch=7;
#else
    const uint32_t arch=0;
#endif
    if(!arch){return 2;}
    unsigned char record[PD_APPROVAL_SIZE];struct pd_approval approval;struct pd_release release;
    if(!pd_approval_read(3,record,&approval) || !pd_release_read(0,pd_release_key,arch,approval.profile,
        approval.firmware,approval.digest,&release)){return 3;}
    umask(077);struct stat st;
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0){return 4;}
    int ok=!fstat(root,&st) && protected_directory(&st);
    int run=ok?openat(root,"run",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;close(root);
    if(run<0){return 4;}
    ok=!fstat(run,&st) && protected_directory(&st) &&
        fstatat(run,"aetherpd-install",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    if(!ok){close(run);return 4;}
    if(mkdirat(run,"aetherpd-install.stage",0700)){
        if(errno!=EEXIST){close(run);return 4;}
        int prior=openat(run,"aetherpd-install.stage",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int repaired=prior>=0 && stage_partial(prior,&release,record) && stage_remove(run,prior) && !fsync(run);
        if(prior>=0){close(prior);}
        if(!repaired || mkdirat(run,"aetherpd-install.stage",0700)){close(run);return 4;}
    }
    int stage=openat(run,"aetherpd-install.stage",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(stage<0){unlinkat(run,"aetherpd-install.stage",AT_REMOVEDIR);close(run);return 4;}
    ok=pd_release_extract(0,stage,pd_release_key,&release) && pd_stage_checkpoint("extracted");
    if(ok){
        int fd=openat(stage,"approved",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        ok=fd>=0 && write(fd,record,sizeof(record))==(ssize_t)sizeof(record) && !fsync(fd);
        if(fd>=0 && close(fd)){ok=0;}
    }
    int published=0;
    if(ok){
        ok=!fsync(stage) && pd_stage_checkpoint("approved") && !renameat(run,"aetherpd-install.stage",run,"aetherpd-install");
        if(ok){published=1;ok=pd_stage_checkpoint("renamed") && !fsync(run);}
    }
    if(!ok && !published){stage_remove(run,stage);}
    close(stage);close(run);return ok?0:5;
}
