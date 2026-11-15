#ifndef AETHER_PD_RUNTIME_H
#define AETHER_PD_RUNTIME_H
#include "pd_migration.h"
#include "pd_upgrade.h"
/* Rollback-only retirement of the fixed RAM layout after service quiescence.
 * No persistent identity, caller-selected path or generic tree removal. Unknown
 * entries, cross-device objects, links and live owned processes are refused. */
int pd_runtime_retire(int store,const unsigned char plan[PD_MIGRATION_SIZE]);
/* Same fixed volatile layout during upgrade quiescence or rollback; requires
 * the authenticated upgrade worker's exclusive lease and matching journal. */
int pd_runtime_upgrade_retire(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE]);
#endif
