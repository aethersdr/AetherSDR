#include "pd_udp_rx.h"
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* SO_TIMESTAMPNS_OLD is 35 on the ARMv7, ARM64 and x86 Linux targets. */
enum { kTimestampNs = 35 };

int pd_udp_timestamp_enable(int fd)
{
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, kTimestampNs, &enabled, sizeof(enabled)) == 0;
}

ssize_t pd_udp_receive(int fd, unsigned char* packet, size_t capacity,
                       int64_t* arrival_ns, int64_t* age_ns)
{
    /* A 32-bit time64 libc can append an expanded timestamp to the original
     * legacy control message, even when SO_TIMESTAMPNS_OLD was requested. */
    union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(8)+CMSG_SPACE(16)]; } control;
    struct iovec iov = { .iov_base = packet, .iov_len = capacity };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = control.bytes; msg.msg_controllen = sizeof(control.bytes);
    *arrival_ns = 0; *age_ns = -1;
    ssize_t bytes = recvmsg(fd, &msg, MSG_DONTWAIT);
    if (bytes <= 0) { return bytes; }
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) { return bytes; }
    for (size_t offset = 0; offset + sizeof(struct cmsghdr) <= msg.msg_controllen;) {
        struct cmsghdr* c = (struct cmsghdr*)(control.bytes + offset);
        if (c->cmsg_len < CMSG_LEN(0) || c->cmsg_len > msg.msg_controllen-offset) { return bytes; }
        offset += CMSG_ALIGN(c->cmsg_len);
        if (c->cmsg_level != SOL_SOCKET || (c->cmsg_type != kTimestampNs && c->cmsg_type != 64)) { continue; }
        int64_t seconds, nanoseconds;
        if (c->cmsg_type == kTimestampNs && c->cmsg_len == CMSG_LEN(8)) {
            int32_t pair[2]; memcpy(pair, CMSG_DATA(c), sizeof(pair));
            seconds = pair[0]; nanoseconds = pair[1];
        } else if (c->cmsg_len == CMSG_LEN(16)) {
            int64_t pair[2]; memcpy(pair, CMSG_DATA(c), sizeof(pair));
            seconds = pair[0]; nanoseconds = pair[1];
        } else { return bytes; }
        if (seconds <= 0 || seconds > (INT64_MAX-999999999)/1000000000 ||
            nanoseconds < 0 || nanoseconds >= 1000000000) { return bytes; }
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now)) { return bytes; }
        *arrival_ns = seconds * 1000000000 + nanoseconds;
        *age_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec - *arrival_ns;
        return bytes;
    }
    return bytes;
}
