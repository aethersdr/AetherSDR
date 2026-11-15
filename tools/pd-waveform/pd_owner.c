#define _GNU_SOURCE
#include "pd_owner.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/capability.h>
#include <unistd.h>

int pd_owner_open(const char* text)
{
    if (!text || !*text) { return -1; }
    unsigned value=0;
    for (const char* p=text;*p;++p) {
        if (*p<'0' || *p>'9' || value>102) { return -2; }
        value=value*10+(unsigned)(*p-'0');
    }
    if (value<3 || value>1023) { return -2; }
    int fd=(int)value;struct stat st;
    int flags=fcntl(fd,F_GETFL);int descriptor_flags=fcntl(fd,F_GETFD);
    if (flags<0 || descriptor_flags<0 || (flags&O_ACCMODE)!=O_RDONLY ||
        fstat(fd,&st) || !S_ISFIFO(st.st_mode) ||
        fcntl(fd,F_SETFD,descriptor_flags|FD_CLOEXEC)) { return -2; }
    return fd;
}

int pd_owner_closed(int fd)
{
    if (fd<0) { return 0; }
    struct pollfd p={fd,0,0};int ready=poll(&p,1,0);
    return (ready<0 && errno!=EINTR) ||
        (ready>0 && (p.revents&(POLLHUP|POLLERR|POLLNVAL)));
}

int pd_owner_unprivileged(void)
{
    struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};
    struct __user_cap_data_struct caps[2]={{0}};
    return geteuid()!=0 && getuid()==geteuid() && !syscall(SYS_capget,&h,caps) &&
        !caps[0].effective && !caps[0].permitted && !caps[0].inheritable &&
        !caps[1].effective && !caps[1].permitted && !caps[1].inheritable;
}
