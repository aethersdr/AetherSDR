#define _GNU_SOURCE
#include "pd_upgrade.h"
#include "pd_bundle.h"
#include "pd_store.h"
#include "pd_transaction.h"
#include "pd_upgrade_state.h"
#include "pd_upgrade_origin.h"
#include "pd_generation.h"
#include "pd_retained.h"
#include "pd_retire.h"
#include "pd_pair.h"
#include "monocypher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PD_UPGRADE_TEST
extern int pd_upgrade_checkpoint(const char* point,unsigned index);
#else
static int pd_upgrade_checkpoint(const char* point,unsigned index){(void)point;(void)index;return 1;}
#endif

static uid_t upgrade_owner(void)
{
#ifdef PD_UPGRADE_TEST
    return getuid();
#else
    return 0;
#endif
}
static int upgrade_directory(int fd)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISDIR(st.st_mode) &&
        st.st_uid==upgrade_owner() && !(st.st_mode&07022);
}
static int upgrade_metadata(int fd,mode_t mode,off_t size)
{
    struct stat st;return fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) &&
        st.st_uid==upgrade_owner() && st.st_nlink==1 && (st.st_mode&07777)==mode && st.st_size==size;
}
static int upgrade_read(int fd,unsigned char* bytes,size_t size)
{
    size_t used=0;
    while(used<size){
        ssize_t n=pread(fd,bytes+used,size-used,(off_t)used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){return 0;}used+=(size_t)n;
    }
    return 1;
}
static int upgrade_file(int directory,const char* name,mode_t mode,const struct pd_release* release,unsigned index)
{
    int fd=openat(directory,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);unsigned char digest[32];
    int ok=upgrade_metadata(fd,mode,release->length[index]);
    crypto_blake2b_ctx hash;crypto_blake2b_init(&hash,32);unsigned char bytes[16384];uint32_t offset=0;
    while(ok && offset<release->length[index]){
        size_t count=release->length[index]-offset;if(count>sizeof(bytes)){count=sizeof(bytes);}
        ssize_t n=pread(fd,bytes,count,offset);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}
        crypto_blake2b_update(&hash,bytes,(size_t)n);offset+=(uint32_t)n;
    }
    crypto_blake2b_final(&hash,digest);
    ok=ok && !crypto_verify32(digest,release->manifest+PD_RELEASE_HEADER+index*PD_RELEASE_RECORD+4);
    if(fd>=0){close(fd);}return ok;
}
static int upgrade_installed(int store,int units,uint32_t arch,const struct pd_upgrade* upgrade)
{
    unsigned char expected[48]={0},marker[48];memcpy(expected,"PDINST01",8);
    expected[11]=(unsigned char)arch;expected[15]=(unsigned char)upgrade->previous.profile;
    memcpy(expected+16,upgrade->previous_key,32);
    int installed=openat(store,"installed",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    int ok=upgrade_metadata(installed,0600,sizeof(marker)) && upgrade_read(installed,marker,sizeof(marker)) &&
        !memcmp(expected,marker,sizeof(marker));if(installed>=0){close(installed);}
    const struct pd_release* previous=&upgrade->previous_release;
    ok=ok && upgrade_file(store,"pd-boot",0755,previous,0) && upgrade_file(store,"BOOT-LICENSES.txt",0644,previous,3);
    int definitions=ok?openat(store,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    ok=ok && upgrade_directory(definitions);
    for(unsigned i=4;ok && i<PD_RELEASE_COUNT;++i){
        ok=upgrade_file(definitions,pd_release_names[i],0644,previous,i) &&
            upgrade_file(units,pd_release_names[i],0644,previous,i);
    }
    if(definitions>=0){close(definitions);}
    struct pd_selection selection;struct pd_bundle payload;
    ok=ok && pd_selection_read(store,&selection)==1 &&
        !crypto_verify32(selection.current_hash,previous->manifest+PD_RELEASE_HEADER+2*PD_RELEASE_RECORD+4);
    int slots=ok?openat(store,"slots",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int file=upgrade_directory(slots)?pd_store_slot(slots,selection.current):-1;unsigned char digest[32];
    ok=ok && file>=0 && pd_bundle_hash(file,digest) && !crypto_verify32(digest,selection.current_hash) &&
        pd_bundle_read(file,upgrade->previous_key,arch,&payload) && pd_bundle_validate(file,&payload);
    if(file>=0){close(file);}if(slots>=0){close(slots);}return ok;
}
static int upgrade_selected(int store,int units,uint32_t arch,const struct pd_identity* identity,
                             const struct pd_upgrade_state* prior,const struct pd_upgrade* next,
                             struct pd_release* installed_management)
{
    if(!pd_upgrade_follows(prior,next) || !pd_retire_ready(store,prior)){return 0;}
    struct pd_generation generation;int selected=pd_generation_active(store,&generation);
    int stage=generation.stage,control=generation.control;
    struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};struct pd_upgrade staged;struct stat partial;
    unsigned choice=prior->phase==PD_UPGRADE_COMMITTED?PD_PAIR_CANDIDATE:PD_PAIR_PREVIOUS;
    int ok=selected==1 && stage>=0 && control>=0 &&
        pd_pair_acquire(control,stage,prior->approval.candidate_key,arch,identity,&pair) && pair.choice==choice &&
        fstatat(control,"active.new",&partial,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT &&
        pd_retained_reopen(store,stage,prior->approval.candidate_key,arch,identity,prior,&staged) &&
        !memcmp(staged.record,prior->approval.record,PD_UPGRADE_APPROVAL_SIZE);
    /* Rollback selects the previous payload but retains candidate management.
     * Verify both against their authenticated predecessor, under the pair lease. */
    const struct pd_release* management=&staged.candidate_release;
    ok=ok && upgrade_file(store,"pd-boot",0755,management,0) &&
        upgrade_file(store,"BOOT-LICENSES.txt",0644,management,3);
    int definitions=ok?openat(store,"units",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    ok=ok && upgrade_directory(definitions);
    for(unsigned i=4;ok && i<PD_RELEASE_COUNT;++i){
        ok=upgrade_file(definitions,pd_release_names[i],0644,management,i) &&
            upgrade_file(units,pd_release_names[i],0644,management,i);
    }
    if(ok){*installed_management=*management;}
    if(definitions>=0){close(definitions);}pd_pair_close(&pair);
    pd_generation_close(&generation);return ok;
}
int pd_upgrade_inspect_origin(int store,int units,int approval,int previous,int candidate,
                       const unsigned char expected_key[32],uint32_t arch,
                       const struct pd_identity* identity,struct pd_upgrade* upgrade,struct pd_upgrade_origin* origin)
{
    unsigned char bytes[PD_UPGRADE_APPROVAL_SIZE];memset(upgrade,0,sizeof(*upgrade));
    if(!upgrade_directory(store) || !upgrade_directory(units) ||
        !upgrade_metadata(approval,0600,sizeof(bytes)) || !upgrade_read(approval,bytes,sizeof(bytes)) ||
        !pd_upgrade_parse(bytes,sizeof(bytes),upgrade) || crypto_verify32(expected_key,upgrade->candidate_key) ||
        !pd_identity_matches(identity,upgrade->candidate.profile,upgrade->candidate.firmware,upgrade->candidate.serial)){return 0;}
    int lease=pd_transaction_session_lock(store);if(lease<0){return 0;}
    struct pd_upgrade_state prior;struct pd_release management;int present=pd_upgrade_state_read(store,&prior);
    int ok=present>=0 && pd_upgrade_admit(store,0) &&
        pd_release_read(previous,upgrade->previous_key,arch,upgrade->previous.profile,
        upgrade->previous.firmware,upgrade->previous.digest,&upgrade->previous_release) &&
        pd_release_read(candidate,upgrade->candidate_key,arch,upgrade->candidate.profile,
        upgrade->candidate.firmware,upgrade->candidate.digest,&upgrade->candidate_release) &&
        (present?upgrade_selected(store,units,arch,identity,&prior,upgrade,&management):upgrade_installed(store,units,arch,upgrade));
    if(ok && origin){
        ok=pd_upgrade_origin_make(upgrade,present?&prior:NULL,present?&management:&upgrade->previous_release,origin);
    }
    close(lease);return ok;
}
int pd_upgrade_inspect(int store,int units,int approval,int previous,int candidate,
                       const unsigned char expected_key[32],uint32_t arch,
                       const struct pd_identity* identity,struct pd_upgrade* upgrade)
{
    return pd_upgrade_inspect_origin(store,units,approval,previous,candidate,expected_key,arch,identity,upgrade,NULL);
}
static int upgrade_empty(int stage)
{
    int copy=openat(stage,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* entries=copy>=0?fdopendir(copy):NULL;if(!entries){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(entries);if(!entry){ok=!errno;break;}
        if(strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")){ok=0;break;}
    }
    closedir(entries);return ok;
}
static int upgrade_header(int directory,const struct pd_release* release)
{
    unsigned char bytes[PD_RELEASE_PREFIX];memcpy(bytes,release->manifest,PD_RELEASE_MANIFEST);
    memcpy(bytes+PD_RELEASE_MANIFEST,release->signature,sizeof(release->signature));
    int fd=openat(directory,"release.header",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0){return 0;}size_t used=0;int ok=1;
    while(used<sizeof(bytes)){
        ssize_t n=write(fd,bytes+used,sizeof(bytes)-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && !fchmod(fd,0600) && !fsync(fd);if(close(fd)){ok=0;}return ok;
}
int pd_upgrade_stage(int stage,int previous,int candidate,const struct pd_upgrade* upgrade)
{
    if(!upgrade_directory(stage) || !upgrade_empty(stage)){return 0;}
    struct pd_upgrade_budget budget;
    if(!pd_upgrade_space(stage,&upgrade->previous_release,&upgrade->candidate_release,&budget)){return 0;}
    int intent=openat(stage,"approved",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(intent<0){return 0;}size_t used=0;int ok=1;
    while(used<sizeof(upgrade->record)){
        ssize_t n=write(intent,upgrade->record+used,sizeof(upgrade->record)-used);
        if(n<0 && errno==EINTR){continue;}if(n<=0){ok=0;break;}used+=(size_t)n;
    }
    ok=ok && pd_upgrade_checkpoint("approval-written",0) && !fchmod(intent,0600) && !fsync(intent);if(close(intent)){ok=0;}
    ok=ok && !fsync(stage) && pd_upgrade_checkpoint("approval-synced",0);
    const char* const names[]={"previous","candidate"};
    for(unsigned i=0;ok && i<2;++i){
        if(mkdirat(stage,names[i],0700) || !pd_upgrade_checkpoint("directory-created",i)){return 0;}
        int directory=openat(stage,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        const struct pd_release* release=i?&upgrade->candidate_release:&upgrade->previous_release;
        ok=upgrade_directory(directory) && upgrade_header(directory,release) && pd_upgrade_checkpoint("header-written",i) &&
            pd_release_extract(i?candidate:previous,directory,
                i?upgrade->candidate_key:upgrade->previous_key,release) && pd_upgrade_checkpoint("release-written",i);
        if(directory>=0){close(directory);}
    }
    return ok && !fsync(stage) && pd_upgrade_checkpoint("stage-synced",0);
}
static int upgrade_inventory(int directory,int top,unsigned required)
{
    const char* const root_names[]={"approved","previous","candidate"};
    unsigned count=top?3:PD_RELEASE_COUNT+1,seen=0;
    int copy=openat(directory,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* entries=copy>=0?fdopendir(copy):NULL;if(!entries){if(copy>=0){close(copy);}return 0;}
    int ok=1;
    for(;;){
        errno=0;struct dirent* entry=readdir(entries);if(!entry){ok=!errno;break;}
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        unsigned index=0;
        for(;index<count;++index){
            const char* name=top?root_names[index]:index<PD_RELEASE_COUNT?pd_release_names[index]:"release.header";
            if(!strcmp(entry->d_name,name)){break;}
        }
        if(index==count || (seen&(1u<<index))){ok=0;break;}
        seen|=1u<<index;
    }
    closedir(entries);return ok && (seen&required)==required;
}
static int upgrade_reopen_release(int stage,const char* name,const unsigned char key[32],uint32_t arch,
                                   const struct pd_approval* approval,struct pd_release* release)
{
    int directory=openat(stage,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat parent,st;
    int ok=upgrade_directory(directory) && !fstat(stage,&parent) && !fstat(directory,&st) &&
        st.st_dev==parent.st_dev && (st.st_mode&07777)==0700 && upgrade_inventory(directory,0,(1u<<(PD_RELEASE_COUNT+1))-1);
    int fd=ok?openat(directory,"release.header",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC):-1;
    unsigned char header[PD_RELEASE_PREFIX],digest[32];
    ok=ok && upgrade_metadata(fd,0600,sizeof(header)) && !fstat(fd,&st) && st.st_dev==parent.st_dev &&
        upgrade_read(fd,header,sizeof(header)) &&
        pd_release_header_parse(header,sizeof(header),key,arch,approval->profile,approval->firmware,release) && !fsync(fd);
    if(fd>=0){close(fd);}
    crypto_blake2b_ctx whole;crypto_blake2b_init(&whole,32);
    if(ok){crypto_blake2b_update(&whole,header,sizeof(header));}
    for(unsigned i=0;ok && i<PD_RELEASE_COUNT;++i){
        fd=openat(directory,pd_release_names[i],O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        ok=upgrade_metadata(fd,i<2?0755:0644,release->length[i]) && !fstat(fd,&st) && st.st_dev==parent.st_dev;
        unsigned char* bytes=ok?malloc(release->length[i]):NULL;
        ok=ok && bytes && upgrade_read(fd,bytes,release->length[i]) &&
            pd_release_record_validate(bytes,release->length[i],i,release);
        if(ok){crypto_blake2b_update(&whole,bytes,release->length[i]);}
        free(bytes);
        if(ok && i==2){
            struct pd_bundle payload;ok=pd_bundle_read(fd,key,arch,&payload) && pd_bundle_validate(fd,&payload);
        }
        if(ok){ok=!fsync(fd);}if(fd>=0){close(fd);}
    }
    crypto_blake2b_final(&whole,digest);
    ok=ok && !crypto_verify32(digest,approval->digest) && !fsync(directory);
    if(directory>=0){close(directory);}return ok;
}
static int upgrade_reopen(int stage,const unsigned char expected_key[32],uint32_t arch,
                           const struct pd_identity* identity,unsigned choice,struct pd_upgrade* upgrade)
{
    memset(upgrade,0,sizeof(*upgrade));
    unsigned required=choice==2?7:1u|(1u<<(choice+1));
    if(!upgrade_directory(stage) || !upgrade_inventory(stage,1,required)){return 0;}
    int approved=openat(stage,"approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    unsigned char record[PD_UPGRADE_APPROVAL_SIZE];struct stat parent,st;
    int ok=upgrade_metadata(approved,0600,sizeof(record)) && !fstat(stage,&parent) && !fstat(approved,&st) &&
        st.st_dev==parent.st_dev && upgrade_read(approved,record,sizeof(record)) &&
        pd_upgrade_parse(record,sizeof(record),upgrade) && !crypto_verify32(expected_key,upgrade->candidate_key) &&
        pd_identity_matches(identity,upgrade->candidate.profile,upgrade->candidate.firmware,upgrade->candidate.serial) &&
        !fsync(approved);
    if(approved>=0){close(approved);}
    return ok && (choice==1 || upgrade_reopen_release(stage,"previous",upgrade->previous_key,arch,&upgrade->previous,&upgrade->previous_release)) &&
        (choice==0 || upgrade_reopen_release(stage,"candidate",upgrade->candidate_key,arch,&upgrade->candidate,&upgrade->candidate_release)) &&
        pd_upgrade_checkpoint("reopen-verified",0) && !fsync(stage);
}
int pd_upgrade_reopen(int stage,const unsigned char expected_key[32],uint32_t arch,
                      const struct pd_identity* identity,struct pd_upgrade* upgrade)
{
    return upgrade_reopen(stage,expected_key,arch,identity,2,upgrade);
}
int pd_upgrade_reopen_choice(int stage,const unsigned char expected_key[32],uint32_t arch,
                             const struct pd_identity* identity,unsigned choice,struct pd_upgrade* upgrade)
{
    if(choice>1){memset(upgrade,0,sizeof(*upgrade));return 0;}
    return upgrade_reopen(stage,expected_key,arch,identity,choice,upgrade);
}
