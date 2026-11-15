#ifndef AETHER_PD_PAIR_H
#define AETHER_PD_PAIR_H
#include "pd_upgrade.h"
enum { PD_PAIR_RECORD_SIZE=80, PD_PAIR_PREVIOUS=0, PD_PAIR_CANDIDATE=1 };
struct pd_pair {
    int lease,boot,payload;
    unsigned choice,boot_handoff;
    unsigned char payload_key[32];
};
/* Linked worker primitives, not a privileged command interface. control is a
 * separate fixed protected directory on the stage filesystem. All stage writes
 * must use its exclusive lease. The caller handles live-target observation,
 * quiescence, recovery authority and transaction phase admission. */
int pd_pair_publish(int control,int stage,const unsigned char expected_key[32],uint32_t arch,
                    const struct pd_identity* identity,unsigned choice);
/* Returns pinned, authenticated boot and payload FDs from one selected release.
 * Keep the shared lease until boot preparation finishes; any exec handoff must
 * explicitly preserve the lease and descriptors. Never reopen a selected path. */
int pd_pair_acquire(int control,int stage,const unsigned char expected_key[32],uint32_t arch,
                    const struct pd_identity* identity,struct pd_pair* pair);
void pd_pair_close(struct pd_pair* pair);
#endif
