#define PD_INSTALL_MAIN inherited_install_main
#include "pd_install_release.c"
#undef PD_INSTALL_MAIN
#include "pd_migration.h"

int main(int argc,char** argv)
{
    (void)argv;if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
#if !defined(__aarch64__)
    return 2;
#else
    unsigned char target[PD_APPROVAL_SIZE],plan[PD_MIGRATION_SIZE];
    struct pd_approval approval;struct pd_migration migration;
    struct pd_release release;struct pd_identity identity;struct pd_predecessor predecessor;
    if(!pd_approval_read(3,target,&approval) || !pd_migration_read(4,plan,&migration) ||
        memcmp(target,migration.approval,PD_APPROVAL_SIZE) ||
        !pd_release_read(0,pd_release_key,64,approval.profile,approval.firmware,approval.digest,&release) ||
        !pd_identity_probe(&identity) || !pd_identity_matches(&identity,release.profile,approval.firmware,approval.serial) ||
        !pd_predecessor_probe(&predecessor) || !pd_migration_matches(&migration,&predecessor) ||
        !pd_predecessor_image_verify(&predecessor)){return 3;}
    struct stat st;if(!lstat("/run/aetherpd",&st) || errno!=ENOENT){return 4;}
    umask(077);int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 4;}
    const char* const parent_parts[]={"var","lib","smartsdr"};
    const char* const unit_parts[]={"etc","systemd","system"};
    const char* const stage_parts[]={"run","aetherpd-install"};
    int parent=install_path(root,parent_parts,3),units=install_path(root,unit_parts,3),stage=install_path(root,stage_parts,2);close(root);
    int ok=parent>=0 && units>=0 && stage>=0 && pd_install_prepare(parent,units,stage,pd_release_key,&release,approval.digest);
    int store=ok?openat(parent,"aetherpd",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    int result=store>=0?pd_migration_copy(store,plan):0;
    if(store>=0){close(store);}if(parent>=0){close(parent);}if(units>=0){close(units);}if(stage>=0){close(stage);}
    if(result==1){puts("{\"copied\":true,\"predecessor_stopped\":true,\"activated\":false,\"removed\":false,\"rf_commands\":[]}");return 0;}
    printf("{\"copied\":false,\"recovery_required\":%s,\"activated\":false,\"removed\":false,\"rf_commands\":[]}\n",result<0?"true":"false");
    return result<0?7:6;
#endif
}
