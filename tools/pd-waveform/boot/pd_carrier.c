#define _GNU_SOURCE
#include "pd_carrier.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <unistd.h>

static int same(const struct stat* a,const struct stat* b)
{
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino;
}
static int network(int proc)
{
    struct stat self,init;
    return !fstatat(proc,"self/ns/net",&self,0) && !fstatat(proc,"1/ns/net",&init,0) && same(&self,&init);
}
int pd_carrier_enter(int root)
{
    struct stat supplied,observed;struct statfs fs;
    if(getuid()!=0 || geteuid()!=0 || fstat(root,&supplied) || !S_ISDIR(supplied.st_mode) ||
       supplied.st_uid!=0 || (supplied.st_mode&0022)){return 0;}
    int proc=openat(root,"proc",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(proc<0){return 0;}
    char self[32],expected[32];ssize_t size=readlinkat(proc,"self",self,sizeof(self));
    int length=snprintf(expected,sizeof(expected),"%ld",(long)getpid());
    int ok=!fstatfs(proc,&fs) && (unsigned long)fs.f_type==0x9fa0UL &&
        length>0 && length<(int)sizeof(expected) && size==length && !memcmp(self,expected,(size_t)length) &&
        !fstatat(proc,"1/root",&observed,0) && same(&supplied,&observed) && network(proc);
    close(proc);
    struct __user_cap_header_struct header={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2];
    if(!ok || syscall(SYS_capget,&header,caps)){return 0;}
    /* DAC/ownership capabilities are used by the fixed installer. PTRACE is
     * needed only for the existing process/executable identity checks. */
    const unsigned allowed=(1U<<CAP_CHOWN)|(1U<<CAP_DAC_OVERRIDE)|(1U<<CAP_FOWNER)|
        (1U<<CAP_SYS_CHROOT)|(1U<<CAP_SYS_PTRACE);
    if(caps[1].effective || caps[1].permitted || caps[0].inheritable || caps[1].inheritable ||
       (caps[0].effective&~allowed) || (caps[0].permitted&~allowed) ||
       !(caps[0].effective&(1U<<CAP_SYS_CHROOT))){return 0;}
    for(unsigned cap=0;cap<64;++cap){
        int present=prctl(PR_CAPBSET_READ,cap,0,0,0);
        if(present<0){if(errno==EINVAL){break;}return 0;}
        if(present && (cap>=32 || !(allowed&(1U<<cap)))){return 0;}
    }
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) || fchdir(root) || chroot(".") || chdir("/")){return 0;}
    caps[0].effective&=~(1U<<CAP_SYS_CHROOT);caps[0].permitted&=~(1U<<CAP_SYS_CHROOT);
    if(syscall(SYS_capset,&header,caps) || syscall(SYS_capget,&header,caps) ||
       (caps[0].effective&(1U<<CAP_SYS_CHROOT)) || (caps[0].permitted&(1U<<CAP_SYS_CHROOT))){return 0;}
    return 1;
}
