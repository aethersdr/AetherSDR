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
    struct pd_release release;struct pd_identity identity;
    if(!pd_approval_read(3,target,&approval) || !pd_migration_read(4,plan,&migration) ||
        memcmp(target,migration.approval,PD_APPROVAL_SIZE) ||
        !pd_release_read(0,pd_release_key,64,approval.profile,approval.firmware,approval.digest,&release) ||
        !pd_identity_probe(&identity) || !pd_identity_matches(&identity,release.profile,approval.firmware,approval.serial)){return 3;}
    umask(077);int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 4;}
    const char* const parts[]={"var","lib","smartsdr","aetherpd"};
    int store=install_path(root,parts,4);close(root);
    int result=store>=0?pd_migration_recover(store,plan):0;if(store>=0){close(store);}
    printf("{\"recovered\":%s,\"recovery_required\":%s,\"activated\":false,\"rf_commands\":[]}\n",result==1?"true":"false",result<0?"true":"false");
    return result==1?0:result<0?7:6;
#endif
}
