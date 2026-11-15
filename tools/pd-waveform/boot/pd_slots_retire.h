#ifndef AETHER_PD_SLOTS_RETIRE_H
#define AETHER_PD_SLOTS_RETIRE_H
#include "pd_upgrade_state.h"
enum { PD_SLOTS_RETIRE_SIZE=8+PD_UPGRADE_APPROVAL_SIZE+48+4+88+3*40+32 };
/* Fixed original A/B/C payload retirement, under store/transaction leases.
 * Requires a complete terminal generation; preserves original installation
 * metadata and private identity. The protected receipt is retained permanently. */
int pd_slots_retire(int store,const unsigned char key[32],uint32_t arch,const struct pd_identity* identity);
#endif
