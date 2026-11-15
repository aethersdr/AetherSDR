#define _GNU_SOURCE
#include "pd_upgrade.h"
#include "pd_store.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifdef PD_UPGRADE_TEST
extern uint64_t pd_upgrade_test_free(uint64_t actual,int inodes);
#else
static uint64_t pd_upgrade_test_free(uint64_t actual,int inodes){(void)inodes;return actual;}
#endif
static int add_file(int directory,const char* name,uint64_t size,uint64_t block,struct pd_upgrade_budget* budget)
{
    struct stat st;uint64_t existing=0;
    if(directory>=0 && !fstatat(directory,name,&st,AT_SYMLINK_NOFOLLOW)){
        if(!S_ISREG(st.st_mode) || st.st_size<0 || (uint64_t)st.st_size>size){return 0;}
        existing=(uint64_t)st.st_size;
    }else{
        if(directory>=0 && errno!=ENOENT){return 0;}
        ++budget->required_inodes;
    }
    if(existing<size){
        /* Charge the boundary block even if already allocated. Existing sparse
         * prefixes are read/verified but never rewritten during resumption. */
        uint64_t blocks=size/block+(size%block!=0)-existing/block;
        if(blocks>(UINT64_MAX-budget->required_bytes)/block){return 0;}
        budget->required_bytes+=blocks*block;
    }
    return 1;
}
int pd_upgrade_space(int stage,const struct pd_release* previous,const struct pd_release* candidate,
                      struct pd_upgrade_budget* budget)
{
    memset(budget,0,sizeof(*budget));struct statvfs space;
    if(fstatvfs(stage,&space) || !space.f_frsize || space.f_bavail>UINT64_MAX/space.f_frsize){return 0;}
    uint64_t block=space.f_frsize;
    if(block>(UINT64_MAX-PD_STORE_RESERVE)/3){return 0;}
    budget->available_bytes=pd_upgrade_test_free((uint64_t)space.f_bavail*block,0);
    budget->available_inodes=pd_upgrade_test_free(space.f_favail,1);
    /* Three directories may grow; the separate reserve covers filesystem
     * metadata and leaves room for recovery after successful staging. */
    budget->required_bytes=PD_STORE_RESERVE+3*block;
    if(!add_file(stage,"approved",PD_UPGRADE_APPROVAL_SIZE,block,budget)){return 0;}
    const char* const names[]={"previous","candidate"};
    const struct pd_release* releases[]={previous,candidate};
    for(unsigned i=0;i<2;++i){
        int directory=openat(stage,names[i],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(directory<0){
            if(errno!=ENOENT){return 0;}++budget->required_inodes;
        }
        int ok=add_file(directory,"release.header",PD_RELEASE_PREFIX,block,budget);
        for(unsigned j=0;ok && j<PD_RELEASE_COUNT;++j){
            ok=add_file(directory,pd_release_names[j],releases[i]->length[j],block,budget);
        }
        if(directory>=0){close(directory);}if(!ok){return 0;}
    }
    return budget->available_bytes>=budget->required_bytes &&
        (!space.f_files || budget->available_inodes>=budget->required_inodes);
}
