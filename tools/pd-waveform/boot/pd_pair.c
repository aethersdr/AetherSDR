#define _GNU_SOURCE
#include "pd_pair.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char* point,unsigned index);
static uid_t pair_owner(void){return getuid();}
#else
static int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
static uid_t pair_owner(void){return 0;}
#endif
static int directory_ok(int fd,dev_t* device)
{
    struct stat st;
    if(fstat(fd,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=pair_owner() || (st.st_mode&07777)!=0700){return 0;}
    *device=st.st_dev;return 1;
}
static int inventory(int control)
{
    int copy=openat(control,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(listing);if(!entry){ok=!errno;break;}
        const char* name=entry->d_name;
        if(strcmp(name,".") && strcmp(name,"..") && strcmp(name,"active") &&
            strcmp(name,"active.new") && strcmp(name,"lock")){ok=0;break;}
    }
    closedir(listing);return ok;
}
static int file_ok(int fd,dev_t device,mode_t mode,off_t size)
{
    struct stat st;return !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==pair_owner() &&
        st.st_nlink==1 && st.st_dev==device && (st.st_mode&07777)==mode && st.st_size==size;
}
static int pair_lock(int control,int stage,int exclusive)
{
    dev_t device,stage_device;
    if(!directory_ok(control,&device) || !directory_ok(stage,&stage_device) || device!=stage_device || !inventory(control)){return -1;}
    int fd=openat(control,"lock",O_RDWR|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|(exclusive?O_CREAT:0),0600);
    if(fd<0){return -1;}
    if(!file_ok(fd,device,0600,0) || flock(fd,(exclusive?LOCK_EX:LOCK_SH)|LOCK_NB) ||
        !inventory(control) || (exclusive && fsync(fd))){
        close(fd);return -1;
    }
    return fd;
}
static void record_make(const struct pd_upgrade* upgrade,unsigned choice,unsigned char record[PD_PAIR_RECORD_SIZE])
{
    memset(record,0,PD_PAIR_RECORD_SIZE);memcpy(record,"PDPAIR01",8);record[8]=(unsigned char)choice;
    crypto_blake2b(record+16,32,upgrade->record,sizeof(upgrade->record));
    memcpy(record+48,choice==PD_PAIR_PREVIOUS?upgrade->previous.digest:upgrade->candidate.digest,32);
}
static int read_record(int control,const char* name,unsigned char bytes[PD_PAIR_RECORD_SIZE],size_t* size,int partial)
{
    dev_t device;struct stat st;
    if(!directory_ok(control,&device)){return -1;}
    int fd=openat(control,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return errno==ENOENT?0:-1;}
    int ok=!fstat(fd,&st) && st.st_size>=0 && st.st_size<=PD_PAIR_RECORD_SIZE &&
        (partial || st.st_size==PD_PAIR_RECORD_SIZE) && file_ok(fd,device,0600,st.st_size);
    *size=ok?(size_t)st.st_size:0;size_t used=0;
    while(ok && used<*size){
        ssize_t n=pread(fd,bytes+used,*size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    close(fd);return ok?1:-1;
}
static int record_choice(const unsigned char bytes[PD_PAIR_RECORD_SIZE],const unsigned char records[2][PD_PAIR_RECORD_SIZE])
{
    if(!memcmp(bytes,records[0],PD_PAIR_RECORD_SIZE)){return PD_PAIR_PREVIOUS;}
    if(!memcmp(bytes,records[1],PD_PAIR_RECORD_SIZE)){return PD_PAIR_CANDIDATE;}
    return -1;
}
int pd_pair_publish(int control,int stage,const unsigned char expected_key[32],uint32_t arch,
                    const struct pd_identity* identity,unsigned choice)
{
    if(choice>PD_PAIR_CANDIDATE){return 0;}
    int lease=pair_lock(control,stage,1);if(lease<0){return 0;}
    struct pd_upgrade upgrade;unsigned char records[2][PD_PAIR_RECORD_SIZE],active[PD_PAIR_RECORD_SIZE],temporary[PD_PAIR_RECORD_SIZE];
    int ok=choice==PD_PAIR_PREVIOUS?pd_upgrade_reopen_choice(stage,expected_key,arch,identity,choice,&upgrade):
        pd_upgrade_reopen(stage,expected_key,arch,identity,&upgrade);
    record_make(&upgrade,0,records[0]);record_make(&upgrade,1,records[1]);size_t length=0;
    int prior=ok?read_record(control,"active",active,&length,0):-1;
    int selected=prior==1?record_choice(active,records):-1;
    ok=ok && (prior==0?choice==PD_PAIR_PREVIOUS:prior==1 && selected>=0);
    int pending=ok?read_record(control,"active.new",temporary,&length,1):-1;
    ok=ok && pending>=0 && (pending==0 || !memcmp(temporary,records[0],length) || !memcmp(temporary,records[1],length));
    if(ok && pending==1){ok=!unlinkat(control,"active.new",0) && !fsync(control);}
    if(ok && selected==(int)choice){
        ok=pd_upgrade_checkpoint("pair-retry",choice) && !fsync(control);close(lease);return ok;
    }
    int fd=ok?openat(control,"active.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600):-1;
    ok=ok && fd>=0;size_t written=0;
    while(ok && written<PD_PAIR_RECORD_SIZE){
        ssize_t n=write(fd,records[choice]+written,PD_PAIR_RECORD_SIZE-written);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}written+=(size_t)n;
    }
    ok=ok && pd_upgrade_checkpoint("pair-written",choice) && !fsync(fd) && pd_upgrade_checkpoint("pair-synced",choice);
    if(fd>=0 && close(fd)){ok=0;}
    ok=ok && !renameat(control,"active.new",control,"active") && pd_upgrade_checkpoint("pair-renamed",choice) &&
        !fsync(control) && pd_upgrade_checkpoint("pair-committed",choice);
    close(lease);return ok;
}
static int pin_member(int directory,dev_t device,const struct pd_release* release,unsigned index)
{
    int fd=openat(directory,pd_release_names[index],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){return -1;}
    int ok=file_ok(fd,device,index<2?0755:0644,release->length[index]);
    unsigned char* bytes=ok?malloc(release->length[index]):NULL;ok=ok && bytes;size_t used=0;
    while(ok && used<release->length[index]){
        ssize_t n=pread(fd,bytes+used,release->length[index]-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && pd_release_record_validate(bytes,release->length[index],index,release);free(bytes);
    if(!ok){close(fd);return -1;}return fd;
}
void pd_pair_close(struct pd_pair* pair)
{
    if(pair->boot>=0){close(pair->boot);}if(pair->payload>=0){close(pair->payload);}if(pair->lease>=0){close(pair->lease);}
    pair->boot=-1;pair->payload=-1;pair->lease=-1;
}
int pd_pair_acquire(int control,int stage,const unsigned char expected_key[32],uint32_t arch,
                    const struct pd_identity* identity,struct pd_pair* pair)
{
    memset(pair,0,sizeof(*pair));pair->boot=-1;pair->payload=-1;pair->lease=pair_lock(control,stage,0);
    if(pair->lease<0){return 0;}
    struct pd_upgrade upgrade;unsigned char records[2][PD_PAIR_RECORD_SIZE],active[PD_PAIR_RECORD_SIZE];size_t length;
    int ok=read_record(control,"active",active,&length,0)==1 && active[8]<=PD_PAIR_CANDIDATE;
    unsigned choice=ok?active[8]:0;
    memset(&upgrade,0,sizeof(upgrade));
    ok=ok && pd_upgrade_reopen_choice(stage,expected_key,arch,identity,choice,&upgrade);
    record_make(&upgrade,0,records[0]);record_make(&upgrade,1,records[1]);
    int selected=ok?record_choice(active,records):-1;
    pair->choice=selected>=0?(unsigned)selected:0;
    int directory=selected>=0?openat(stage,selected==PD_PAIR_PREVIOUS?"previous":"candidate",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    dev_t device,stage_device;ok=ok && selected>=0 && directory_ok(stage,&stage_device) && directory_ok(directory,&device) && device==stage_device;
    const struct pd_release* release=selected==PD_PAIR_PREVIOUS?&upgrade.previous_release:&upgrade.candidate_release;
    pair->boot_handoff=ok?release->boot_handoff:0;
    if(ok){
        pair->boot=pin_member(directory,device,release,0);pair->payload=pin_member(directory,device,release,2);
        memcpy(pair->payload_key,selected==PD_PAIR_PREVIOUS?upgrade.previous_key:upgrade.candidate_key,32);
    }
    if(directory>=0){close(directory);}ok=ok && pair->boot>=0 && pair->payload>=0;
    if(!ok){pd_pair_close(pair);}return ok;
}
