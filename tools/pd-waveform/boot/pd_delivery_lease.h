#ifndef AETHER_PD_DELIVERY_LEASE_H
#define AETHER_PD_DELIVERY_LEASE_H
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* FD7 keeps the delivery directory leased across exec and supervisor death.
 * Keep it for this worker's lifetime, but never pass it to executed helpers. */
static int pd_delivery_lease_adopt(void)
{
    struct stat st;
#ifdef PD_DELIVERY_TEST
    uid_t owner=getuid();
#else
    uid_t owner=0;
#endif
    return !fstat(7,&st) && S_ISDIR(st.st_mode) && st.st_uid==owner &&
        (st.st_mode&07777)==0700 && !flock(7,LOCK_EX|LOCK_NB) &&
        !fcntl(7,F_SETFD,FD_CLOEXEC);
}
#endif
