#define PD_STAGE_MAIN inherited_stage_main
#include "pd_stage_release.c"
#undef PD_STAGE_MAIN
#include "pd_install.h"
#include "pd_identity.h"

static int install_path(int root,const char* const* parts,size_t count)
{
    int fd=fcntl(root,F_DUPFD_CLOEXEC,4);if(fd<0){return -1;}
    for(size_t i=0;i<count;++i){
        int next=openat(fd,parts[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=next;
        struct stat st;
        if(fd<0 || fstat(fd,&st) || !protected_directory(&st)){if(fd>=0){close(fd);}return -1;}
    }
    return fd;
}
#ifndef PD_INSTALL_MAIN
#define PD_INSTALL_MAIN main
#endif
int PD_INSTALL_MAIN(int argc,char** argv)
{
    (void)argv;if(argc!=1 || getuid()!=0 || geteuid()!=0){return 2;}
#if defined(__aarch64__)
    const uint32_t arch=64;const char* const parent_parts[]={"var","lib","smartsdr"};
#elif defined(__arm__)
    const uint32_t arch=7;const char* const parent_parts[]={"user"};
#else
    const uint32_t arch=0;const char* const parent_parts[]={"user"};
#endif
    if(!arch){return 2;}
    unsigned char record[PD_APPROVAL_SIZE];struct pd_approval approval;
    struct pd_release release;struct pd_identity identity;
    if(!pd_approval_read(3,record,&approval) || !pd_release_read(0,pd_release_key,arch,approval.profile,
        approval.firmware,approval.digest,&release) || !pd_identity_probe(&identity) ||
        !pd_identity_matches(&identity,release.profile,approval.firmware,approval.serial)){return 3;}
    struct stat st;
    if(!lstat("/run/aetherpd",&st) || errno!=ENOENT){return 4;}
    umask(077);int root=open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0 || fstat(root,&st) || !protected_directory(&st)){if(root>=0){close(root);}return 4;}
    const char* const units_parts[]={"etc","systemd","system"};
    const char* const stage_parts[]={"run","aetherpd-install"};
    int parent=install_path(root,parent_parts,sizeof(parent_parts)/sizeof(parent_parts[0]));
    int units=install_path(root,units_parts,3),stage=install_path(root,stage_parts,2);close(root);
    int ok=parent>=0 && units>=0 && stage>=0;
    if(ok){ok=pd_install_prepare(parent,units,stage,pd_release_key,&release,approval.digest);}
    if(parent>=0){close(parent);}if(units>=0){close(units);}if(stage>=0){close(stage);}
    if(ok){puts("{\"prepared\":true,\"activation_changed\":false,\"started\":false,\"rf_commands\":[]}");}
    return ok?0:5;
}
