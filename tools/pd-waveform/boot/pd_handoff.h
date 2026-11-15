#ifndef AETHER_PD_HANDOFF_H
#define AETHER_PD_HANDOFF_H
#include "pd_pair.h"
enum { PD_HANDOFF_LEASE=64, PD_HANDOFF_PAYLOAD=65, PD_HANDOFF_BOOT=66 };
/* Consumes the caller's descriptor environment and cwd. Only call in a process
 * that exits on failure. The pair must come from pd_pair_acquire; no paths or
 * arbitrary command/environment arguments are accepted. */
int pd_handoff_exec(const struct pd_pair* pair,int socket_owner);
/* In the executed verifier, authenticate the inherited payload against its
 * compiled key, self executable and fixed control lease. Returns a new owned
 * payload FD. Descriptor64 keeps selection leased until this process exits. */
int pd_handoff_receive(int control,const unsigned char key[32],uint32_t arch);
#endif
