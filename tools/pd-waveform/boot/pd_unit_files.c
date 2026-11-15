#define _GNU_SOURCE
#include "pd_unit_files.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char kWants[]="multi-user.target.wants";
#ifdef PD_WORKER_UNITS_TEST
static uid_t unit_owner(void){return getuid();}
#else
static uid_t unit_owner(void){return 0;}
#endif
#ifdef PD_WORKER_UNITS_FAULTS
extern int pd_worker_units_checkpoint(const char* point);
#else
static int pd_worker_units_checkpoint(const char* point){(void)point;return 1;}
#endif
static int unit_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) && st.st_uid==unit_owner() && !(st.st_mode&022);
}
static int unit_file(int parent,const char* name,const char* expected,int partial)
{
    int fd=openat(parent,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    struct stat st;size_t length=strlen(expected);char bytes[2048];
    int ok=length<=sizeof(bytes) && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==unit_owner() && st.st_nlink==1 &&
        st.st_size>=0 && st.st_size<=(off_t)length && (partial || st.st_size==(off_t)length) &&
        ((st.st_mode&07777)==0644 || (partial && (st.st_mode&07777)==0600)) &&
        pread(fd,bytes,(size_t)st.st_size,0)==st.st_size && !memcmp(bytes,expected,(size_t)st.st_size);
    close(fd);return ok?1:-1;
}
static int unit_link(int wants,const char* name,const struct pd_unit_files* definitions)
{
    struct stat st;char value[128],target[128];
    int n=snprintf(target,sizeof(target),"../%s",definitions->names[1]);
    if(n<0 || (size_t)n>=sizeof(target)){return -1;}
    if(fstatat(wants,name,&st,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT?0:-1;}
    return S_ISLNK(st.st_mode) && st.st_uid==unit_owner() && st.st_nlink==1 &&
        readlinkat(wants,name,value,sizeof(value))==(ssize_t)strlen(target) && !memcmp(value,target,strlen(target))?1:-1;
}
static int unit_wants(int units,int create)
{
    if(create && mkdirat(units,kWants,0755) && errno!=EEXIST){return -1;}
    int fd=openat(units,kWants,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0 && !unit_directory(fd)){close(fd);errno=EINVAL;return -1;}return fd;
}
static int unit_preflight(int units,int wants,const struct pd_unit_files* definitions)
{
    for(unsigned i=0;i<2;++i){
        char partial[80],dropins[80];struct stat st;
        snprintf(partial,sizeof(partial),".%s.new",definitions->names[i]);
        snprintf(dropins,sizeof(dropins),"%s.d",definitions->names[i]);
        if(unit_file(units,definitions->names[i],definitions->text[i],0)<0 ||
            unit_file(units,partial,definitions->text[i],1)<0 ||
            !fstatat(units,dropins,&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){return 0;}
    }
    if(wants<0){return 1;}
    struct stat st;
    char partial[96];snprintf(partial,sizeof(partial),".%s.new",definitions->names[1]);
    return unit_link(wants,definitions->names[1],definitions)>=0 && unit_link(wants,partial,definitions)>=0 &&
        fstatat(wants,definitions->names[0],&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
}
static int unit_publish(int units,unsigned index,const struct pd_unit_files* definitions)
{
    const char* name=definitions->names[index];const char* text=definitions->text[index];
    char partial[80];snprintf(partial,sizeof(partial),".%s.new",name);
    if(unit_file(units,name,text,0)==1){
        if(unlinkat(units,partial,0) && errno!=ENOENT){return 0;}return !fsync(units);
    }
    int fd=openat(units,partial,O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t size=strlen(text),used=0;int ok=1;
    while(used<size){
        ssize_t n=pwrite(fd,text+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fchmod(fd,0644) && !fsync(fd);if(close(fd)){ok=0;}
    char point[80];snprintf(point,sizeof(point),"unit-file:%u",index);
    ok=ok && pd_worker_units_checkpoint(point) && !renameat(units,partial,units,name);
    snprintf(point,sizeof(point),"unit-rename:%u",index);
    return ok && pd_worker_units_checkpoint(point) && !fsync(units);
}
int pd_unit_files_verify(int units,const struct pd_unit_files* definitions)
{
    if(!unit_directory(units)){return 0;}int wants=unit_wants(units,0);
    char partial[96];snprintf(partial,sizeof(partial),".%s.new",definitions->names[1]);
    int ok=wants>=0 && unit_preflight(units,wants,definitions) && unit_link(wants,definitions->names[1],definitions)==1;
    for(unsigned i=0;ok && i<2;++i){
        char partial[80];snprintf(partial,sizeof(partial),".%s.new",definitions->names[i]);
        ok=unit_file(units,definitions->names[i],definitions->text[i],0)==1 && unit_file(units,partial,definitions->text[i],1)==0;
    }
    ok=ok && unit_link(wants,partial,definitions)==0;
    if(wants>=0){close(wants);}return ok;
}
int pd_unit_files_arm(int units,const struct pd_unit_files* definitions)
{
    if(!unit_directory(units)){return 0;}int wants=unit_wants(units,0);
    if(wants<0 && errno!=ENOENT){return 0;}
    char partial[96],target[96];
    snprintf(partial,sizeof(partial),".%s.new",definitions->names[1]);snprintf(target,sizeof(target),"../%s",definitions->names[1]);
    int ok=unit_preflight(units,wants,definitions);if(wants>=0){close(wants);}if(!ok){return 0;}
    for(unsigned i=0;ok && i<2;++i){ok=unit_publish(units,i,definitions);}
    wants=ok?unit_wants(units,1):-1;ok=ok && wants>=0;
    if(ok && unit_link(wants,definitions->names[1],definitions)==0){
        int staged=unit_link(wants,partial,definitions);
        ok=staged==1 || (staged==0 && !symlinkat(target,wants,partial));
        ok=ok && !fsync(wants) && pd_worker_units_checkpoint("link-staged") &&
            !renameat(wants,partial,wants,definitions->names[1]) && pd_worker_units_checkpoint("link-published");
    }
    if(ok && unlinkat(wants,partial,0) && errno!=ENOENT){ok=0;}
    ok=ok && !fsync(wants) && !fsync(units) && pd_worker_units_checkpoint("armed") && pd_unit_files_verify(units,definitions);
    if(wants>=0){close(wants);}return ok;
}
int pd_unit_files_remove(int units,const struct pd_unit_files* definitions)
{
    if(!unit_directory(units)){return 0;}
    char partial[96];snprintf(partial,sizeof(partial),".%s.new",definitions->names[1]);
    int wants=unit_wants(units,0);if(wants<0 && errno!=ENOENT){return 0;}
    int ok=unit_preflight(units,wants,definitions);
    if(ok && wants>=0){
        ok=(!unlinkat(wants,definitions->names[1],0) || errno==ENOENT) && (!unlinkat(wants,partial,0) || errno==ENOENT) && !fsync(wants);
    }
    if(wants>=0){close(wants);}ok=ok && pd_worker_units_checkpoint("disabled");
    for(unsigned i=0;ok && i<2;++i){
        char partial[80];snprintf(partial,sizeof(partial),".%s.new",definitions->names[i]);
        ok=(!unlinkat(units,definitions->names[i],0) || errno==ENOENT) && (!unlinkat(units,partial,0) || errno==ENOENT);
        char point[80];snprintf(point,sizeof(point),"removed:%u",i);ok=ok && pd_worker_units_checkpoint(point);
    }
    return ok && !fsync(units);
}
