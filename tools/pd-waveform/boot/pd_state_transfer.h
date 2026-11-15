#ifndef AETHER_PD_STATE_TRANSFER_H
#define AETHER_PD_STATE_TRANSFER_H
#include "pd_approval.h"
/* Internal installer operation, not a request interface. The caller must
 * authenticate the release/target and quiesce the admitted predecessor before
 * supplying its two fixed state descriptors. Source descriptors are read only.
 * A durable prepared snapshot or completed transfer can resume without them. */
int pd_state_transfer(int store,int provision,int tailscale,
                      const unsigned char approval[PD_APPROVAL_SIZE]);
/* Reverse preparation for the 8000 rollback adapter: quiesced UID78666 inputs
 * become a complete root-owned private tree before any container is created.
 * The caller supplies a distinct owned rollback store, never the primary store.
 * Direction-specific journals prevent replaying a forward transfer as restore. */
int pd_state_restore_legacy(int store,int provision,int tailscale,
                            const unsigned char approval[PD_APPROVAL_SIZE]);
#endif
