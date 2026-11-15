#define _GNU_SOURCE
#include "pd_install.h"
#include "pd_approval.h"
#include "pd_store.h"
#include "pd_bundle.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_INSTALL_TEST
static uid_t install_owner(void){return getuid();}
static uid_t install_data_owner(void){return getuid();}
static gid_t install_data_group(void){return getgid();}
static int install_quiet(void){return 1;}
extern int pd_install_checkpoint(const char* point);
#else
static uid_t install_owner(void){return 0;}
static uid_t install_data_owner(void){return 78666;}
static gid_t install_data_group(void){return 20;}
static int install_quiet(void){struct stat st;return lstat("/run/aetherpd",&st)<0 && errno==ENOENT;}
static int pd_install_checkpoint(const char* point){(void)point;return 1;}
#endif
static int install_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) &&
        st.st_uid==install_owner() && !(st.st_mode&022);
}
static int install_empty(int fd)
{
    DIR* listing=fdopendir(dup(fd));if(!listing){return 0;}
    int empty=1;struct dirent* entry;errno=0;
    while((entry=readdir(listing))){
        if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")){empty=0;break;}
    }
    if(errno){empty=0;}closedir(listing);return empty;
}
static int install_open(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0 && !install_directory(fd)){close(fd);errno=EINVAL;return -1;}return fd;
}
static int install_mkdir(int parent,const char* name)
{
    int made=!mkdirat(parent,name,0755);
    if(!made && errno!=EEXIST){return -1;}
    int fd=install_open(parent,name);
    if(fd>=0 && made && fchmod(fd,0755)){close(fd);return -1;}return fd;
}
static int install_file(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd>=0 && (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=install_owner() ||
        st.st_nlink!=1 || (st.st_mode&06022))){close(fd);errno=EINVAL;return -1;}return fd;
}
static int install_write(int fd,const unsigned char* bytes,size_t size)
{
    while(size){
        ssize_t n=write(fd,bytes,size);if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        bytes+=n;size-=(size_t)n;
    }
    return 1;
}
static int install_equal(int fd,const unsigned char* bytes,size_t size,mode_t mode)
{
    struct stat st;unsigned char actual[128];
    return size<=sizeof(actual) && !fstat(fd,&st) && st.st_size==(off_t)size &&
        (st.st_mode&07777)==mode && pread(fd,actual,size,0)==(ssize_t)size && !memcmp(actual,bytes,size);
}
static int install_marker(int parent,const char* name,const unsigned char* bytes,size_t size)
{
    int old=install_file(parent,name);
    if(old>=0){int ok=install_equal(old,bytes,size,0600);close(old);return ok;}
    if(errno!=ENOENT){return 0;}
    char temporary[64];int n=snprintf(temporary,sizeof(temporary),"%s.new",name);
    if(n<=0 || n>=(int)sizeof(temporary)){return 0;}
    int prior=install_file(parent,temporary);
    if(prior>=0){
        struct stat st;int safe=!fstat(prior,&st) && st.st_size>=0 && st.st_size<=(off_t)size && (st.st_mode&07777)==0600;close(prior);
        if(!safe || unlinkat(parent,temporary,0)){return 0;}
    }else if(errno!=ENOENT){return 0;}
    int fd=openat(parent,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}int ok=install_write(fd,bytes,size) && !fsync(fd);if(close(fd)){ok=0;}
    return ok && !renameat(parent,temporary,parent,name) && !fsync(parent);
}
static int install_matches(int fd,const struct pd_release* release,unsigned index,mode_t mode)
{
    struct stat st;unsigned char bytes[16384],digest[32];
    if(fstat(fd,&st) || st.st_size!=release->length[index] || (st.st_mode&07777)!=mode){return 0;}
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);uint32_t offset=0;
    while(offset<release->length[index]){
        size_t count=release->length[index]-offset;if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(fd,bytes,count,offset);if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}
        crypto_blake2b_update(&hash,bytes,(size_t)n);offset+=(uint32_t)n;
    }
    crypto_blake2b_final(&hash,digest);
    return !crypto_verify32(digest,release->manifest+PD_RELEASE_HEADER+index*PD_RELEASE_RECORD+4);
}
static int install_copy(int stage,int dest,const struct pd_release* release,unsigned index)
{
    const char* name=pd_release_names[index];mode_t mode=index==0?0755:0644;
    int source=install_file(stage,name);if(source<0){return 0;}
    int ok=install_matches(source,release,index,index<2?0755:0644);
    int existing=install_file(dest,name);
    if(existing>=0){ok=ok && install_matches(existing,release,index,mode);close(existing);close(source);return ok;}
    if(!ok || errno!=ENOENT){close(source);return 0;}
    char temporary[80];snprintf(temporary,sizeof(temporary),"%s.new",name);
    int old=install_file(dest,temporary);
    if(old>=0){
        struct stat st;ok=!fstat(old,&st) && st.st_size>=0 && st.st_size<=release->length[index] &&
            ((st.st_mode&07777)==0600 || install_matches(old,release,index,mode));
        close(old);if(ok){ok=!unlinkat(dest,temporary,0);}
    }else{ok=errno==ENOENT;}
    int output=ok?openat(dest,temporary,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600):-1;
    if(output<0){close(source);return 0;}
    unsigned char bytes[16384];uint32_t offset=0;
    while(ok && offset<release->length[index]){
        size_t count=release->length[index]-offset;if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(source,bytes,count,offset);if(n<0 && errno==EINTR){continue;}
        if(n<=0 || !install_write(output,bytes,(size_t)n)){ok=0;break;}offset+=(uint32_t)n;
    }
    close(source);ok=ok && install_matches(output,release,index,0600) && !fchmod(output,mode) && !fsync(output);
    if(close(output)){ok=0;}
    return ok && !renameat(dest,temporary,dest,name) && !fsync(dest);
}
static int install_units(int stage,int units,const struct pd_release* release)
{
    /* Unit discovery precedes mounting the payload partition. Keep the signed
     * definitions directly in systemd's unit directory, not linked into it. */
    for(unsigned i=4;i<PD_RELEASE_COUNT;++i){
        if(!install_copy(stage,units,release,i) || !pd_install_checkpoint("unit-published")){return 0;}
    }
    return 1;
}
static int install_units_available(int units,const struct pd_release* release,int existing)
{
    for(unsigned i=4;i<PD_RELEASE_COUNT;++i){
        struct stat st;
        if(fstatat(units,pd_release_names[i],&st,AT_SYMLINK_NOFOLLOW)){
            if(errno==ENOENT){continue;}return 0;
        }
        if(!existing){return 0;}
        int file=install_file(units,pd_release_names[i]);
        if(file<0){return 0;}
        int matches=install_matches(file,release,i,0644);close(file);
        if(!matches){return 0;}
    }
    if(!existing){
        const char* const targets[]={"multi-user.target.wants","sockets.target.wants"};
        for(unsigned t=0;t<2;++t){
            int directory=install_open(units,targets[t]);
            if(directory<0){if(errno==ENOENT){continue;}return 0;}
            int ok=1;
            for(unsigned i=4;i<PD_RELEASE_COUNT;++i){
                struct stat st;
                if(!fstatat(directory,pd_release_names[i],&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT){ok=0;break;}
            }
            close(directory);if(!ok){return 0;}
        }
    }
    return 1;
}
int pd_install_prepare(int parent,int units,int stage,const unsigned char key[32],
                       const struct pd_release* release,const unsigned char approved[32])
{
    if(!install_directory(parent) || !install_directory(units) || !install_directory(stage)){return 0;}
    unsigned char marker[48]={0};memcpy(marker,"PDINST01",8);marker[11]=(unsigned char)release->arch;
    marker[15]=(unsigned char)release->profile;memcpy(marker+16,key,32);
    int store=install_open(parent,"aetherpd"),pending=0;
    if(store<0 && errno!=ENOENT){return 0;}
    if(store>=0){
        int installed=install_file(store,"installed");
        int recognized=installed>=0 && install_equal(installed,marker,sizeof(marker),0600);
        if(installed>=0){close(installed);}if(!recognized){close(store);return 0;}
    }
    if(!install_units_available(units,release,store>=0)){if(store>=0){close(store);}return 0;}
    if(store<0){
        store=install_mkdir(parent,".aetherpd-prepare");pending=1;
    }
    if(store<0){return 0;}
    if(pending){
        int intent=install_file(store,"intent");
        int recognized=intent>=0 && install_equal(intent,approved,32,0600);
        if(intent>=0){close(intent);}
        if(!recognized){
            if(intent>=0 || errno!=ENOENT){close(store);return 0;}
            DIR* listing=fdopendir(dup(store));if(!listing){close(store);return 0;}
            int empty=1;struct dirent* entry;
            while((entry=readdir(listing))){
                if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..") &&
                    strcmp(entry->d_name,"store.lock") && strcmp(entry->d_name,"intent.new")){empty=0;break;}
            }
            closedir(listing);if(!empty){close(store);return 0;}
        }
    }
    int lock=pd_store_lock(store);if(lock<0){close(store);return 0;}
    if(!install_quiet()){close(lock);close(store);return 0;}
    int ok=(!pending || install_marker(store,"intent",approved,32)) &&
        install_marker(store,"installed",marker,sizeof(marker)) && pd_install_checkpoint("intent-written");
    int slots=ok?install_mkdir(store,"slots"):-1,definitions=ok?install_mkdir(store,"units"):-1;
    if(slots>=0){close(slots);}ok=ok && definitions>=0;
    if(ok){ok=install_copy(stage,store,release,0) && install_copy(stage,store,release,3);}
    for(unsigned i=4;ok && i<PD_RELEASE_COUNT;++i){ok=install_copy(stage,definitions,release,i);}
    if(definitions>=0){close(definitions);}ok=ok && pd_install_checkpoint("fixed-files-written");
    int payload=ok?install_file(stage,"payload.bundle"):-1;
    ok=ok && payload>=0 && install_matches(payload,release,2,0644) && pd_store_update(store,payload,key,release->arch);
    if(payload>=0){close(payload);}ok=ok && pd_install_checkpoint("payload-selected");
    if(ok){
        int made=!mkdirat(store,"state",0700);
        if(!made && errno!=EEXIST){ok=0;}
        int state=ok?openat(store,"state",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;struct stat st;
        int repair=state>=0 && !fstat(state,&st) && st.st_uid==install_owner() &&
            (st.st_mode&07777)==0700 && install_empty(state);
        ok=state>=0 && (!(made || repair) || (!fchown(state,install_data_owner(),install_data_group()) && !fchmod(state,0700))) &&
            !fstat(state,&st) && st.st_uid==install_data_owner() && st.st_gid==install_data_group() && (st.st_mode&07777)==0700;
        if(state>=0){if(fsync(state)){ok=0;}close(state);}
    }
    ok=ok && !fsync(store) && pd_install_checkpoint("store-synced");
    if(ok && pending){ok=!renameat(parent,".aetherpd-prepare",parent,"aetherpd") && !fsync(parent);}
    ok=ok && pd_install_checkpoint("store-published");
    if(ok){ok=install_units(stage,units,release);}
    close(lock);close(store);return ok;
}

int pd_install_stage_discard(int run,const struct pd_release* release,const unsigned char approved[140])
{
    if(!install_directory(run)){return 0;}int stage=install_open(run,"aetherpd-install");
    if(stage<0){return errno==ENOENT;}
    int listing_fd=openat(stage,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=listing_fd>=0?fdopendir(listing_fd):NULL;
    if(!listing){if(listing_fd>=0){close(listing_fd);}close(stage);return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(listing);if(!entry){ok=!errno;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        unsigned index=0;for(;index<PD_RELEASE_COUNT && strcmp(entry->d_name,pd_release_names[index]);++index){}
        int fd=install_file(stage,entry->d_name);
        if(fd<0){ok=0;break;}
        if(index<PD_RELEASE_COUNT){ok=install_matches(fd,release,index,index<2?0755:0644);}
        else{
            struct stat st;unsigned char bytes[PD_APPROVAL_SIZE];
            ok=!strcmp(entry->d_name,"approved") && !fstat(fd,&st) && st.st_size==PD_APPROVAL_SIZE &&
                (st.st_mode&07777)==0600 && pread(fd,bytes,sizeof(bytes),0)==sizeof(bytes) && !memcmp(bytes,approved,sizeof(bytes));
        }
        close(fd);if(!ok){break;}
    }
    closedir(listing);
    for(unsigned i=0;ok && i<PD_RELEASE_COUNT;++i){if(unlinkat(stage,pd_release_names[i],0) && errno!=ENOENT){ok=0;}}
    if(ok && unlinkat(stage,"approved",0) && errno!=ENOENT){ok=0;}
    struct stat a,b;
    ok=ok && !fstat(stage,&a) && !fstatat(run,"aetherpd-install",&b,AT_SYMLINK_NOFOLLOW) &&
        a.st_dev==b.st_dev && a.st_ino==b.st_ino && !unlinkat(run,"aetherpd-install",AT_REMOVEDIR) && !fsync(run);
    close(stage);return ok;
}
