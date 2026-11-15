#ifndef AETHER_PD_GENERATION_H
#define AETHER_PD_GENERATION_H
#include "pd_upgrade_origin.h"
enum { PD_GENERATION_RECORD_SIZE=40 };
struct pd_generation {
    int directory,stage,control;
    int legacy;
};
/* These are fixed worker/boot primitives, not a privileged request interface.
 * Caller holds the store lease throughout access. Generation names are derived
 * from parsed approvals; no caller-supplied pathname is used. */
int pd_generation_open(int store,const struct pd_upgrade* approved,int create,struct pd_generation* generation);
/* 1 selected pair, 0 original installation without a pair, -1 invalid/pending.
 * Retains support for the fixed upgrade-pair/upgrade-control layout. */
int pd_generation_active(int store,struct pd_generation* generation);
/* Publish only an authenticated previous selection during QUIESCING/ROLLBACK
 * under the exclusive worker lease. Origin must come from its trusted capsule.
 * A matching partial selector resumes; a foreign selector remains untouched. */
int pd_generation_select(int store,const struct pd_upgrade* approved,const struct pd_upgrade_origin* origin,
                          uint32_t arch,const struct pd_identity* identity);
void pd_generation_close(struct pd_generation* generation);
#endif
