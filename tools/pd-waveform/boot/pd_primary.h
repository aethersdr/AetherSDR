#ifndef AETHER_PD_PRIMARY_H
#define AETHER_PD_PRIMARY_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "pd_readiness.h"
struct pd_primary {
    int process,executable,features;
    pid_t pid;
    uint64_t started,listener;
};
/* Linked worker operation. PID comes from the verified fixed systemd service;
 * executable is an already signature-authenticated protected payload descriptor.
 * This function compares the executing inode, not a caller-supplied hash/path.
 * Neither this API nor the parsers are a privileged request interface. */
int pd_primary_open(pid_t pid,int executable,struct pd_primary* primary);
int pd_primary_check(const struct pd_primary* primary);
/* Bound HTTP readiness by the same process/start tick/listener on both sides.
 * The caller must also recheck the fixed service's MainPID before commitment. */
int pd_primary_wait(const struct pd_primary* primary,const char* address,const struct pd_readiness* expected);
void pd_primary_close(struct pd_primary* primary);
/* Bounded parsers for kernel-provided metadata, also exercised without sockets.
 * features: mask1 requires NoNewPrivs; mask2 requires ambient capability status. */
int pd_primary_status(const unsigned char* bytes,size_t size,pid_t pid,unsigned features);
int pd_primary_stat(const unsigned char* bytes,size_t size,pid_t pid,uint64_t* started);
int pd_primary_cgroup(const unsigned char* bytes,size_t size);
/* Accumulate at most one wildcard listener at48992 across tcp and tcp6.
 * Any conflicting address/owner/second listener refuses admission. */
int pd_primary_tcp(const unsigned char* bytes,size_t size,int ipv6,uint64_t* inode);
#endif
