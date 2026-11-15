#ifndef AETHER_PD_UPGRADE_UNITS_H
#define AETHER_PD_UPGRADE_UNITS_H
#include "pd_upgrade.h"
/* Caller authenticates the protected capsule, holds its lease and validates
 * the canonical unit directory. Recovery directly executes the fixed capsule
 * worker so arming does not depend on the installed boot-helper generation. */
int pd_upgrade_units_arm(int units);
int pd_upgrade_units_verify(int units);
/* Caller holds the capsule namespace lease. Remove only after matching terminal
 * state and removal of active authority. Production callers use finish_cleanup. */
int pd_upgrade_units_disarm(int units,int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE]);
/* Permanent boot cleanup after the temporary executable has been detached.
 * Nonterminal state is left to the independent recovery worker. */
int pd_upgrade_finish_cleanup(int parent,int units,int store,const unsigned char key[32]);
int pd_upgrade_reconcile_terminal(int parent,int units,int store,const unsigned char key[32]);
#endif
