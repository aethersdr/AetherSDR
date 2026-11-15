#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { PD_START_GATE_CANCELED=1, PD_START_GATE_READABLE=2, PD_START_GATE_ERROR=4 };
struct pd_start_gate_io {
    int64_t (*now_ms)(void);
    int (*owner_closed)(int owner);
    ssize_t (*send_ready)(int fd,const void* bytes,size_t count);
    ssize_t (*receive_release)(int fd,void* bytes,size_t count);
    int (*wait)(int fd,int owner,int milliseconds);
};
/* One private pre-API initialization barrier, not a runtime control endpoint.
 * 1: released; 0: owner ended; -1: malformed, blocked, failed or timed out. */
int pd_start_gate_wait_io(int fd,int owner,const struct pd_start_gate_io* io);
int pd_start_gate_wait(int fd,int owner);
