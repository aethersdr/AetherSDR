#ifndef AETHER_PD_FIRST_H
#define AETHER_PD_FIRST_H
#include "pd_migration.h"
#include "pd_readiness.h"
/* Called only inside the authenticated installation transaction. Does not
 * copy an identity or contact a tailnet; publishes a fresh local credential hash. */
int pd_first_initialize(int store,const struct pd_migration* plan);
/* Called only after the worker authenticated the selected payload/readiness. */
int pd_first_select(int store,const unsigned char plan[PD_MIGRATION_SIZE]);
int pd_first_readiness(const struct pd_migration* plan,struct pd_readiness* ready);
#endif
