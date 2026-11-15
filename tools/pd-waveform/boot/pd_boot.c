#define main inherited_shim_main
#include "../session/pd_shim_launch.c"
#undef main
#include "pd_bundle.h"
#include "pd_store.h"
#include "pd_transaction.h"
#include "pd_capsule.h"
#include "pd_upgrade_units.h"
#include "pd_handoff.h"
#include "pd_generation.h"
#include "pd_boot_wait.h"
#include "pd_release_key.h"
#include "monocypher.h"
#include <sys/file.h>

int pd_boot_primary(int argc,char** argv);
int pd_boot_session(int argc,char** argv);

#if defined(__aarch64__)
static const uint32_t kBundleArch=64;
static const char* const kStoreParts[]={"var","lib","smartsdr","aetherpd"};
#elif defined(__arm__)
static const uint32_t kBundleArch=7;
static const char* const kStoreParts[]={"user","aetherpd"};
#else
/* Host builds exercise rejection and parser tests; they are not radio packages. */
static const uint32_t kBundleArch=0;
static const char* const kStoreParts[]={"user","aetherpd"};
#endif
static int boot_directory(int parent,const char* name)
{
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd<0){return -1;}
    if(fstat(fd,&st) || !protected_directory(&st)){close(fd);return -1;}return fd;
}
static int boot_store(int root)
{
    int fd=fcntl(root,F_DUPFD_CLOEXEC,3);if(fd<0){return -1;}
    for(size_t i=0;i<sizeof(kStoreParts)/sizeof(kStoreParts[0]);++i){
        int next=boot_directory(fd,kStoreParts[i]);close(fd);if(next<0){return -1;}fd=next;
    }
    return fd;
}
static int boot_runtime(int root)
{
    int run=boot_directory(root,"run");if(run<0){return -1;}
    int created=!mkdirat(run,"aetherpd",0755);
    if(!created && errno!=EEXIST){close(run);return -1;}
    int fd=boot_directory(run,"aetherpd");close(run);
    if(fd>=0 && created && fchmod(fd,0755)){close(fd);return -1;}
    return fd;
}
static int boot_data(int parent,const char* name)
{
    int created=!mkdirat(parent,name,0700);
    if(!created && errno!=EEXIST){return 0;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0){return 0;}
    struct stat st;
    int ok=(!created || (!fchown(fd,78666,20) && !fchmod(fd,0700))) &&
        !fstat(fd,&st) && data_directory(&st);
    close(fd);return ok;
}
static int boot_selected(int store,unsigned char digest[32])
{
    struct pd_selection select;
    if(pd_selection_read(store,&select)!=1){return -1;}
    int slots=boot_directory(store,"slots");if(slots<0){return -1;}
    int fd=pd_store_slot(slots,select.current);close(slots);
    if(fd<0){return -1;}
    if(!pd_bundle_hash(fd,digest) || crypto_verify32(digest,select.current_hash)){close(fd);return -1;}
    return fd;
}
static int boot_stage_remove(int runtime)
{
    int fd=openat(runtime,".stage",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0){return errno==ENOENT;}
    struct stat st;
    if(fstat(fd,&st) || !protected_directory(&st)){close(fd);return 0;}
    DIR* listing=fdopendir(dup(fd));if(!listing){close(fd);return 0;}
    int ok=1;struct dirent* entry;
    while((entry=readdir(listing))){
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")){continue;}
        int known=0;for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){known|=!strcmp(entry->d_name,pd_bundle_names[i]);}
        if(!known || fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 || (st.st_mode&06022)){ok=0;break;}
    }
    closedir(listing);
    if(ok){
        for(unsigned i=0;i<PD_BUNDLE_COUNT;++i){
            if(unlinkat(fd,pd_bundle_names[i],0) && errno!=ENOENT){ok=0;break;}
        }
    }
    close(fd);return ok && !unlinkat(runtime,".stage",AT_REMOVEDIR);
}
static int boot_socket_owner(int runtime)
{
    struct stat st;
    if(fstatat(runtime,"start.sock",&st,AT_SYMLINK_NOFOLLOW) || !S_ISSOCK(st.st_mode) ||
        st.st_nlink!=1 || (st.st_mode&07777)!=0600 ||
        !((st.st_uid==0 && st.st_gid==0) || (st.st_uid==78666 && st.st_gid==20))){return 0;}
    return !fchownat(runtime,"start.sock",78666,20,AT_SYMLINK_NOFOLLOW);
}
static int boot_prepare_key(int store,int runtime,int inherited_payload,const unsigned char payload_key[32])
{
    unsigned char digest[32];
    int fd=inherited_payload>=0?fcntl(inherited_payload,F_DUPFD_CLOEXEC,3):boot_selected(store,digest);
    if(fd<0){return 0;}
    struct pd_bundle bundle;
    if(!pd_bundle_read(fd,payload_key,kBundleArch,&bundle)){
        fputs("AetherPD boot: payload authentication or format refused\n",stderr);close(fd);return 0;
    }
    int bin=openat(runtime,"bin",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int ok=0;
    if(bin>=0){
        struct stat st;ok=!fstat(bin,&st) && protected_directory(&st) && pd_bundle_matches(bin,&bundle);
        if(!ok){fputs("AetherPD boot: existing RAM payload differs; stop and retire it before update\n",stderr);}
    }else if(errno==ENOENT && boot_stage_remove(runtime) && !mkdirat(runtime,".stage",0700)){
        int stage=boot_directory(runtime,".stage");
        if(stage>=0){
            ok=pd_bundle_extract(fd,stage,&bundle) && !fchmod(stage,0755) && !fsync(stage);
            close(stage);
            if(ok){ok=!renameat(runtime,".stage",runtime,"bin") && !fsync(runtime);}
            if(ok){bin=boot_directory(runtime,"bin");ok=bin>=0;}
        }
        if(!ok){boot_stage_remove(runtime);}
    }
    close(fd);
    if(ok){ok=boot_data(store,"state") && boot_data(runtime,"primary") && boot_data(bin,"state") && boot_data(bin,"run");}
    if(bin>=0){close(bin);}return ok;
}
static int boot_prepare(int store,int runtime,int inherited_payload)
{
    return boot_prepare_key(store,runtime,inherited_payload,pd_release_key);
}
static int boot_installation(int root,int store,int recovery)
{
    if(kBundleArch!=64){return 2;}
    int parent=boot_directory(store,".."),etc=boot_directory(root,"etc");
    int systemd=etc>=0?boot_directory(etc,"systemd"):-1;
    int units=systemd>=0?boot_directory(systemd,"system"):-1;
    if(etc>=0){close(etc);}if(systemd>=0){close(systemd);}
    if(parent<0 || units<0){if(parent>=0){close(parent);}if(units>=0){close(units);}return 4;}
    struct stat st;int absent=fstatat(parent,"aetherpd-installation",&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;
    if(absent){
        unsigned char plan[PD_MIGRATION_SIZE];struct pd_migration parsed;
        int journal=openat(store,"migration-approved",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        int ok=pd_migration_read(journal,plan,&parsed) && pd_capsule_finish_cleanup(parent,units,store,pd_release_key,plan);
        if(journal>=0){close(journal);}close(units);close(parent);return ok?0:4;
    }
    close(units);struct pd_capsule capsule;
    if(!pd_capsule_open(parent,pd_release_key,&capsule)){
        int result=errno==EWOULDBLOCK?4:3;close(parent);return result;
    }
    /* The capsule verifier pins and authenticates this exact executable. The
     * worker checks its own inode again after exec and probes live identity. */
    int worker=fcntl(capsule.worker,F_DUPFD_CLOEXEC,64);
    pd_capsule_close(&capsule);close(parent);
    if(worker<0){return 4;}
    int kept[6]={0,1,2,0,1,2};
    if(chdir("/") || !close_unrelated(kept,worker)){close(worker);return 4;}
    char* args[]={"aetherpd-install-worker",recovery?"--recover":NULL,NULL};
    char* env[]={"PATH=/usr/bin:/bin",NULL};
    fexecve(worker,args,env);close(worker);return 4;
}
static int boot_reconcile(int root,int store)
{
    int parent=boot_directory(store,".."),etc=boot_directory(root,"etc");
    int systemd=etc>=0?boot_directory(etc,"systemd"):-1;
    int units=systemd>=0?boot_directory(systemd,"system"):-1;
    int ok=parent>=0 && units>=0 && pd_capsule_reconcile_terminal(parent,units,store,pd_release_key) &&
        pd_upgrade_reconcile_terminal(parent,units,store,pd_release_key);
    if(units>=0){close(units);}if(systemd>=0){close(systemd);}if(etc>=0){close(etc);}if(parent>=0){close(parent);}
    return ok;
}
static int boot_dispatch(int root,int store,int socket_owner)
{
    int lock=boot_reconcile(root,store)?pd_store_lock(store):-1,lease=-1;
    int admitted=lock>=0 && pd_transaction_runtime_gate(store,&lease);
    struct pd_generation generation={.directory=-1,.stage=-1,.control=-1};
    int selected=admitted?pd_generation_active(store,&generation):-1;
    struct pd_identity identity;struct pd_pair pair={.lease=-1,.boot=-1,.payload=-1};
    int ok=admitted && selected==1 && pd_boot_identity_wait(store,&identity,!socket_owner) &&
        pd_pair_acquire(generation.control,generation.stage,pd_release_key,kBundleArch,&identity,&pair);
    /* Legacy boot code cannot enforce upgrade admission. The permanent loader
     * retains both leases and verifies the selected payload with its approved
     * generation's key instead of executing the legacy verifier. */
    int final_lease=-1;
    ok=ok && pd_transaction_runtime_gate(store,&final_lease);
    int runtime=ok?boot_runtime(root):-1;
    ok=runtime>=0 && (socket_owner?boot_socket_owner(runtime):boot_prepare_key(store,runtime,pair.payload,pair.payload_key));
    if(runtime>=0){close(runtime);}pd_pair_close(&pair);
    if(final_lease>=0){close(final_lease);}
    pd_generation_close(&generation);
    if(lease>=0){close(lease);}if(lock>=0){close(lock);}return ok?0:4;
}
#ifndef PD_BOOT_MAIN
#define PD_BOOT_MAIN main
#endif
int PD_BOOT_MAIN(int argc,char** argv)
{
    int socket_owner=argc==2 && !strcmp(argv[1],"--socket-owner");
    int install=argc==2 && !strcmp(argv[1],"--install-approved");
    int recover=argc==2 && !strcmp(argv[1],"--recover-installation");
    int primary=argc==2 && !strcmp(argv[1],"--primary");
    int session=argc==2 && !strcmp(argv[1],"--session");
    const char* handoff=getenv("AETHERPD_PAIRED_BOOT");
    if((argc!=1 && !socket_owner && !install && !recover && !primary && !session) || getuid()!=0 || geteuid()!=0 || !kBundleArch){return 2;}
    if(handoff && (strcmp(handoff,"1") || install || recover || primary || session)){return 2;}
    umask(077);
    if(primary){return pd_boot_primary(1,argv);}
    if(session){return pd_boot_session(1,argv);}
    int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 3;}
    int store=boot_store(root);
    if(install || recover){
        int result=store>=0?boot_installation(root,store,recover):4;
        if(store>=0){close(store);}close(root);return result;
    }
    int inherited_payload=-1;
    if(handoff){
        struct pd_generation generation={.directory=-1,.stage=-1,.control=-1};
        int selected=store>=0?pd_generation_active(store,&generation):-1;
        inherited_payload=selected==1?pd_handoff_receive(generation.control,pd_release_key,kBundleArch):-1;
        pd_generation_close(&generation);
        if(inherited_payload<0){if(store>=0){close(store);}close(root);return 3;}
    }else if(store>=0){
        const char* const markers[]={"upgrade-control","upgrade-generation","upgrade-generation.new","upgrade-transaction"};
        for(unsigned i=0;i<sizeof(markers)/sizeof(markers[0]);++i){
            struct stat control;
            if(!fstatat(store,markers[i],&control,AT_SYMLINK_NOFOLLOW)){
                int result=boot_dispatch(root,store,socket_owner);close(store);close(root);return result;
            }
            if(errno!=ENOENT){close(store);close(root);return 3;}
        }
    }
    int reconciled=store>=0 && boot_reconcile(root,store);
    int lock=reconciled?pd_store_lock(store):-1;
    int lease=-1,admitted=lock>=0 && pd_transaction_runtime_gate(store,&lease);
    int runtime=admitted?boot_runtime(root):-1;close(root);
    if(store<0 || runtime<0){if(store>=0){close(store);}if(lock>=0){close(lock);}if(lease>=0){close(lease);}if(inherited_payload>=0){close(inherited_payload);}return 3;}
    int ok=admitted && (socket_owner?boot_socket_owner(runtime):boot_prepare(store,runtime,inherited_payload));
    if(inherited_payload>=0){close(inherited_payload);}
    if(lock>=0){close(lock);}if(lease>=0){close(lease);}close(store);close(runtime);return ok?0:4;
}
