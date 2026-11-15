#ifndef AETHER_PD_RETIRE_H
#define AETHER_PD_RETIRE_H
#include "pd_upgrade_origin.h"
enum { PD_RETIRE_RECORD_SIZE=16+PD_UPGRADE_ORIGIN_SIZE+23*40+32 };
/* Retire only the predecessor captured by the authenticated recovery capsule.
 * Requires a terminal selected successor and exclusive transaction/store leases.
 * A protected fixed-file manifest is durable before any predecessor deletion. */
int pd_retire_predecessor(int store,const unsigned char key[32],uint32_t arch,
                           const struct pd_identity* identity,const struct pd_upgrade_origin* origin);
/* A later upgrade must not discard unfinished predecessor-retirement evidence. */
int pd_retire_ready(int store,const struct pd_upgrade_state* predecessor);
#endif
