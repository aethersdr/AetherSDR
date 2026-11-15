#ifndef AETHER_PD_UPGRADE_BOOTSTRAP_H
#define AETHER_PD_UPGRADE_BOOTSTRAP_H
#include "pd_management.h"
/* Fixed bootstrap entry, after independent executable authentication and
 * operator consent. FD3 is the protected approval, FD4/5 the signed releases.
 * Generation-aware release inputs must be on tmpfs, bounded by the release
 * parser's size limits. No runtime service is stopped here; an armed independent
 * worker owns that. */
int pd_upgrade_bootstrap(int root,int parent,int units,int store,int self,uint32_t arch,
                          const unsigned char key[32],const struct pd_management_blob* management);
#endif
