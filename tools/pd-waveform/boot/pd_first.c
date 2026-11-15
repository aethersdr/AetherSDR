#define _GNU_SOURCE
#include "pd_first.h"
#include "pd_store.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_WORKER_TEST
static uid_t first_uid(void){return getuid();}
static gid_t first_gid(void){return getgid();}
#else
static uid_t first_uid(void){return 78666;}
static gid_t first_gid(void){return 20;}
#endif

static void hex(const unsigned char bytes[32],char out[65])
{
    const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}out[64]=0;
}
int pd_first_select(int store,const unsigned char plan[PD_MIGRATION_SIZE])
{
    unsigned char bytes[128]={0},old[128];struct stat st;
    memcpy(bytes,"PDFSEL01",8);crypto_blake2b(bytes+8,32,plan,PD_MIGRATION_SIZE);
    int active=openat(store,"active",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int ok=active>=0 && !fstat(active,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() &&
        st.st_nlink==1 && (st.st_mode&07777)==0600 && st.st_size==PD_SELECTOR_SIZE &&
        pread(active,bytes+40,PD_SELECTOR_SIZE,0)==PD_SELECTOR_SIZE;
    if(active>=0){close(active);}if(!ok){return 0;}
    const char* names[]={"first-selection",".first-selection.new"};
    int present=0;
    for(unsigned i=0;i<2;++i){
        int fd=openat(store,names[i],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if(fd<0){if(errno!=ENOENT){return 0;}continue;}
        ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() && st.st_nlink==1 &&
            (st.st_mode&07777)==0600 && st.st_size>=0 && st.st_size<=128 && (i || st.st_size==128) &&
            pread(fd,old,(size_t)st.st_size,0)==st.st_size && !memcmp(old,bytes,(size_t)st.st_size);
        close(fd);if(!ok){return 0;}
        if(i){if(unlinkat(store,names[i],0)){return 0;}}else{present=1;}
    }
    if(present){return !fsync(store);}
    int fd=openat(store,names[1],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    ok=fd>=0 && write(fd,bytes,sizeof(bytes))==sizeof(bytes) && !fsync(fd);
    if(fd>=0 && close(fd)){ok=0;}
    return ok && !renameat(store,names[1],store,names[0]) && !fsync(store);
}
int pd_first_readiness(const struct pd_migration* plan,struct pd_readiness* ready)
{
    memset(ready,0,sizeof(*ready));if(!plan->fresh){return 0;}
    memcpy(ready->hostname,plan->hostname,64);memcpy(ready->dns,"local:",6);hex(plan->nonce,ready->dns+6);
    return 1;
}
static int first_file(int state,const char* name,const char* expected,size_t length,int partial)
{
    int fd=openat(state,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    struct stat st;char bytes[512];
    int metadata=!fstat(fd,&st);
    int owned=metadata && ((st.st_uid==first_uid() && st.st_gid==first_gid()) ||
        (partial && st.st_uid==0 && st.st_gid==0 && st.st_size==0));
    int ok=owned && S_ISREG(st.st_mode) &&
        st.st_nlink==1 && (st.st_mode&07777)==0600 && st.st_size>=0 && st.st_size<=(off_t)length &&
        (partial || st.st_size==(off_t)length) && length<=sizeof(bytes) &&
        pread(fd,bytes,(size_t)st.st_size,0)==st.st_size && !memcmp(bytes,expected,(size_t)st.st_size);
    close(fd);return ok?1:-1;
}
int pd_first_initialize(int store,const struct pd_migration* plan)
{
    if(!plan->fresh){return 0;}
    int state=openat(store,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(state<0){return 0;}
    int ok=!fstat(state,&st) && st.st_uid==first_uid() && st.st_gid==first_gid() && (st.st_mode&07777)==0700;
    DIR* entries=ok?fdopendir(openat(state,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC)):NULL;
    if(!entries){close(state);return 0;}
    struct dirent* entry;errno=0;
    while((entry=readdir(entries))){
        if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..") && strcmp(entry->d_name,"provision.json") &&
           strcmp(entry->d_name,".first-provision")){ok=0;break;}
    }
    if(errno){ok=0;}closedir(entries);
    char token[65],nonce[65],json[512];hex(plan->token_hash,token);hex(plan->nonce,nonce);
    int length=snprintf(json,sizeof(json),"{\"token_sha256\":\"%s\",\"hostname\":\"%s\",\"installation_id\":\"%s\","
        "\"local_only\":true,\"share_discovered\":false,\"allow\":[],\"routes\":[]}\n",token,plan->hostname,nonce);
    ok=ok && length>0 && length<(int)sizeof(json);
    int present=ok?first_file(state,"provision.json",json,(size_t)length,0):-1;
    int partial=ok?first_file(state,".first-provision",json,(size_t)length,1):-1;
    ok=ok && present>=0 && partial>=0;
    if(ok && partial){ok=!unlinkat(state,".first-provision",0) && !fsync(state);}
    if(ok && !present){
        int file=openat(state,".first-provision",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        ok=file>=0 && !fchown(file,first_uid(),first_gid()) && !fchmod(file,0600);
        size_t used=0;
        while(ok && used<(size_t)length){
            ssize_t n=write(file,json+used,(size_t)length-used);if(n<0 && errno==EINTR){continue;}
            if(n<=0){ok=0;break;}used+=(size_t)n;
        }
        if(ok){ok=!fsync(file);}if(file>=0 && close(file)){ok=0;}
        ok=ok && !renameat(state,".first-provision",state,"provision.json") && !fsync(state);
    }
    close(state);return ok;
}
