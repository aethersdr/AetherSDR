#ifndef AETHER_PD_RETAINED_H
#define AETHER_PD_RETAINED_H
#include "pd_upgrade_state.h"
enum { PD_RETAINED_SIZE=8+PD_UPGRADE_STATE_SIZE+2*PD_RELEASE_PREFIX };
/* Trusted terminal receipt, not an operator credential. Creation authenticates
 * both complete releases before durably binding their headers to the journal.
 * Only the unselected payload may then be removed. Caller owns the authenticated
 * capsule and exclusive transaction lease; selected pair stays pinned. */
int pd_retained_compact(int store,const unsigned char key[32],uint32_t arch,
                         const struct pd_identity* identity);
/* Caller authenticates the terminal predecessor/selection and holds its leases.
 * Full pairs retain the old path. Compacted pairs require the protected receipt,
 * complete selected release and every retained member of the other release. */
int pd_retained_reopen(int store,int stage,const unsigned char key[32],uint32_t arch,
                        const struct pd_identity* identity,const struct pd_upgrade_state* prior,
                        struct pd_upgrade* upgrade);
#endif
