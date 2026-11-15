#define _GNU_SOURCE
#include "pd_runtime.h"
#include "pd_transaction.h"
#include "pd_upgrade_state.h"
#include "pd_service.h"
#include "pd_bundle.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { kEntries=256,kLeafMax=16*1024*1024 };
enum layout { ROOT,BIN,STAGE,DATA,TSNET,PROFILES,PROFILE,CACHE };
struct entry { int parent,fd;char name[256];struct stat observed; };
struct inventory { struct entry entries[kEntries];unsigned count;dev_t device; };
#ifdef PD_RUNTIME_TEST
extern int pd_runtime_test_root(void);
extern int pd_runtime_test_quiet(void);
extern int pd_runtime_checkpoint(void);
static uid_t runtime_owner(int data){(void)data;return getuid();}
static gid_t runtime_group(void){return getgid();}
#else
static int pd_runtime_test_root(void){return open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
static int pd_runtime_test_quiet(void){return pd_service_runtime_quiet();}
static int pd_runtime_checkpoint(void){return 1;}
static uid_t runtime_owner(int data){return data?78666:0;}
static gid_t runtime_group(void){return 20;}
#endif
static int protected_dir(const struct stat* st)
{
    return S_ISDIR(st->st_mode) && st->st_uid==runtime_owner(0) && !(st->st_mode&07022);
}
static int directory(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd>=0 && (fstat(fd,&st) || !protected_dir(&st))){close(fd);errno=EINVAL;return -1;}return fd;
}
static int expected_kind(enum layout layout,const char* name,enum layout* child,int* data)
{
    *data=0;
    if(layout==ROOT){
        if(!strcmp(name,"bin")){*child=BIN;return 1;}
        if(!strcmp(name,".stage")){*child=STAGE;return 1;}
        if(!strcmp(name,"primary")){*child=DATA;*data=1;return 1;}
        return !strcmp(name,"start.sock")?3:0;
    }
    if(layout==BIN || layout==STAGE){
        if(layout==BIN && (!strcmp(name,"state") || !strcmp(name,"run"))){*child=DATA;*data=1;return 1;}
        for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){if(!strcmp(name,pd_bundle_names[i])){return 2;}}
        return 0;
    }
    *data=1;
    if(layout==DATA && !strcmp(name,"tsnet")){*child=TSNET;return 1;}
    if(layout==TSNET && !strcmp(name,"profile-data")){*child=PROFILES;return 1;}
    if(layout==PROFILES){
        /* The bundled Tailscale profile manager uses two random bytes encoded
         * as four lowercase hex digits for each local profile ID. */
        if(strlen(name)!=4 || strspn(name,"0123456789abcdef")!=4){return 0;}
        *child=PROFILE;return 1;
    }
    if(layout==PROFILE){
        if(strcmp(name,"netmap-cache")){return 0;}
        *child=CACHE;return 1;
    }
    /* Only ordinary-user regular leaves are allowed in these three fixed
     * volatile data locations. Tailscale chooses its cache/log basenames. */
    return 2;
}
static int collect(struct inventory* inventory,int parent,enum layout layout)
{
    int copy=openat(parent,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR* listing=copy>=0?fdopendir(copy):NULL;if(!listing){if(copy>=0){close(copy);}return 0;}
    int ok=1;struct dirent* next;
    for(;;){
        errno=0;next=readdir(listing);if(!next){ok=!errno;break;}
        if(!strcmp(next->d_name,".") || !strcmp(next->d_name,"..")){continue;}
        if(inventory->count==kEntries || strlen(next->d_name)>=sizeof(inventory->entries[0].name)){ok=0;break;}
        enum layout child=ROOT;int data=0,kind=expected_kind(layout,next->d_name,&child,&data);struct stat st;
        if(!kind || fstatat(parent,next->d_name,&st,AT_SYMLINK_NOFOLLOW) || st.st_dev!=inventory->device){ok=0;break;}
        if(kind==1){
            ok=S_ISDIR(st.st_mode) && st.st_uid==runtime_owner(data) &&
                (data?(st.st_gid==runtime_group() && (st.st_mode&07777)==0700):protected_dir(&st));
        }else if(kind==3){
            ok=S_ISSOCK(st.st_mode) && st.st_nlink==1 && (st.st_mode&07777)==0600 &&
                (st.st_uid==runtime_owner(0) || (st.st_uid==runtime_owner(1) && st.st_gid==runtime_group()));
        }else{
            ok=S_ISREG(st.st_mode) && st.st_nlink==1 && st.st_uid==runtime_owner(data) && st.st_size>=0 &&
                st.st_size<=(data?kLeafMax:PD_BUNDLE_EXPANDED_MAX) && !(st.st_mode&07022) &&
                (!data || (st.st_gid==runtime_group() && !(st.st_mode&0177)));
        }
        if(!ok){break;}
        struct entry* entry=&inventory->entries[inventory->count++];
        entry->parent=parent;entry->fd=-1;entry->observed=st;strcpy(entry->name,next->d_name);
        if(kind==1){
            entry->fd=openat(parent,entry->name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat pinned;
            ok=entry->fd>=0 && !fstat(entry->fd,&pinned) && pinned.st_dev==st.st_dev && pinned.st_ino==st.st_ino &&
                collect(inventory,entry->fd,child);if(!ok){break;}
        }
    }
    closedir(listing);return ok;
}
static int remove_inventory(struct inventory* inventory)
{
    for(unsigned i=inventory->count;i>0;--i){
        struct entry* entry=&inventory->entries[i-1];struct stat st;
        if(fstatat(entry->parent,entry->name,&st,AT_SYMLINK_NOFOLLOW) || st.st_dev!=entry->observed.st_dev ||
            st.st_ino!=entry->observed.st_ino || st.st_mode!=entry->observed.st_mode || st.st_uid!=entry->observed.st_uid ||
            st.st_gid!=entry->observed.st_gid || (!S_ISDIR(st.st_mode) && st.st_nlink!=1) ||
            unlinkat(entry->parent,entry->name,S_ISDIR(st.st_mode)?AT_REMOVEDIR:0) || !pd_runtime_checkpoint()){return 0;}
    }
    return 1;
}
static int retirement_phase(int store,const unsigned char* approval,int upgrade)
{
    if(!upgrade){return pd_transaction_at_phase(store,approval,PD_TRANSACTION_ROLLBACK);}
    return pd_upgrade_at_phase(store,approval,PD_UPGRADE_QUIESCING) ||
        pd_upgrade_at_phase(store,approval,PD_UPGRADE_ROLLBACK);
}
static int retire(int store,const unsigned char* approval,int upgrade)
{
    if(!retirement_phase(store,approval,upgrade)){return 0;}
#ifndef PD_RUNTIME_TEST
    if(!pd_service_quiesce()){return 0;}
#endif
    if(!pd_runtime_test_quiet()){return 0;}
    int root=pd_runtime_test_root();struct stat st;
    int run=root>=0 && !fstat(root,&st) && protected_dir(&st)?directory(root,"run"):-1;
    if(root>=0){close(root);}if(run<0){return 0;}
    int runtime=directory(run,"aetherpd");
    if(runtime<0){int absent=errno==ENOENT;close(run);return absent;}
    struct inventory inventory={0};struct stat original={0};
    int ok=!fstat(runtime,&original);inventory.device=original.st_dev;
    ok=ok && collect(&inventory,runtime,ROOT) && pd_runtime_test_quiet() &&
        retirement_phase(store,approval,upgrade) && remove_inventory(&inventory) &&
        !fstatat(run,"aetherpd",&st,AT_SYMLINK_NOFOLLOW) && st.st_dev==original.st_dev && st.st_ino==original.st_ino &&
        !unlinkat(run,"aetherpd",AT_REMOVEDIR) && !fsync(run);
    for(unsigned i=0;i<inventory.count;++i){if(inventory.entries[i].fd>=0){close(inventory.entries[i].fd);}}
    close(runtime);close(run);return ok;
}
int pd_runtime_retire(int store,const unsigned char plan[PD_MIGRATION_SIZE])
{
    return retire(store,plan,0);
}
int pd_runtime_upgrade_retire(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE])
{
    return retire(store,approval,1);
}
