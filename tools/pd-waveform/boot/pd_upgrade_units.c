#define _GNU_SOURCE
#include "pd_upgrade_units.h"
#include "pd_upgrade_state.h"
#include "pd_upgrade_capsule.h"
#include "pd_unit_files.h"
#include "pd_upgrade_unit_text.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__arm__) && !defined(__aarch64__)
static const struct pd_unit_files definitions={pd_upgrade_unit_names,pd_upgrade_unit_armv7};
#else
static const struct pd_unit_files definitions={pd_upgrade_unit_names,pd_upgrade_unit_arm64};
#endif
int pd_upgrade_units_arm(int units)
{
    return pd_unit_files_arm(units,&definitions);
}
int pd_upgrade_units_verify(int units)
{
    return pd_unit_files_verify(units,&definitions);
}
static int temporary_authority_absent(int store)
{
    int parent=openat(store,"..",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(parent<0){return 0;}int ok=1;
    const char* const names[]={"aetherpd-upgrade",".aetherpd-upgrade",".aetherpd-upgrade-retired"};
    for(unsigned i=0;ok && i<3;++i){ok=fstatat(parent,names[i],&st,AT_SYMLINK_NOFOLLOW)<0 && errno==ENOENT;}
    close(parent);return ok;
}
int pd_upgrade_units_disarm(int units,int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE])
{
    struct pd_upgrade_state state;
    return pd_upgrade_state_read(store,&state)==1 &&
        (state.phase==PD_UPGRADE_COMMITTED || state.phase==PD_UPGRADE_ROLLED_BACK) &&
        !memcmp(state.approval.record,approval,PD_UPGRADE_APPROVAL_SIZE) &&
        pd_upgrade_admit(store,0) && temporary_authority_absent(store) && !fsync(store) && pd_unit_files_remove(units,&definitions);
}

int pd_upgrade_finish_cleanup(int parent,int units,int store,const unsigned char key[32])
{
    return pd_upgrade_capsule_cleanup_units(parent,units,store,key);
}
int pd_upgrade_reconcile_terminal(int parent,int units,int store,const unsigned char key[32])
{
    struct pd_upgrade_state state;int present=pd_upgrade_state_read(store,&state);
    if(present<=0){return present==0;}
    if(state.phase!=PD_UPGRADE_COMMITTED && state.phase!=PD_UPGRADE_ROLLED_BACK){return 1;}
    return pd_upgrade_finish_cleanup(parent,units,store,key);
}
