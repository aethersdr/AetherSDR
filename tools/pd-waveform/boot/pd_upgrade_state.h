#ifndef AETHER_PD_UPGRADE_STATE_H
#define AETHER_PD_UPGRADE_STATE_H
#include "pd_upgrade.h"
enum pd_upgrade_phase {
    PD_UPGRADE_QUIESCING=1, PD_UPGRADE_SWITCHING=2, PD_UPGRADE_ACTIVATING=3,
    PD_UPGRADE_COMMITTED=4, PD_UPGRADE_ROLLBACK=5, PD_UPGRADE_RESTORING=6,
    PD_UPGRADE_ROLLED_BACK=7
};
enum { PD_UPGRADE_STATE_SIZE=8+PD_UPGRADE_APPROVAL_SIZE+4 };
struct pd_upgrade_state {struct pd_upgrade approval;unsigned phase;};
struct pd_upgrade_ops {
    void* context;
    int (*quiesce)(void*);
    int (*select)(void*);
    int (*activate)(void*);
    int (*rollback)(void*);
    int (*restore)(void*);
    int (*cleanup)(void*);
};
/* 1 present, 0 absent, -1 invalid. No file is created by read/admission. */
int pd_upgrade_state_read(int store,struct pd_upgrade_state* state);
int pd_upgrade_state_parse(const unsigned char* bytes,size_t size,struct pd_upgrade_state* state);
/* A new approval must name exactly the release/key selected by the terminal
 * predecessor. This establishes lineage, not authentication of staged bytes. */
int pd_upgrade_follows(const struct pd_upgrade_state* prior,const struct pd_upgrade* next);
/* Read-only worker preflight before stopping services to release session
 * leases. The caller holds the authenticated capsule namespace lease. */
int pd_upgrade_worker_admit(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE]);
/* Successor requires the exact terminal predecessor captured in the trusted
 * capsule origin, in addition to ordinary installation/authority gates. */
int pd_upgrade_worker_admit_predecessor(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                                       const struct pd_upgrade_state* predecessor);
/* Fixed adapter gate. Requires the matching phase, committed installation,
 * recovery capsule and a live exclusive transaction owner. No mutation. */
int pd_upgrade_at_phase(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],unsigned phase);
/* runtime_pending requires an exclusive worker (checked by the caller), an
 * activation/restoration phase and retained recovery authority. Normal startup
 * requires terminal/no upgrade state and no remaining temporary authority. */
int pd_upgrade_admit(int store,int runtime_pending);
/* 1 committed, 0 restored, -1 recovery/cleanup pending, -2 refused. Uses the
 * existing transaction.lock, without changing the installation journal. The
 * caller authenticates capsule/self, pair and target before invoking callbacks.
 * Re-entry before commit restores the previous release; recovery never advances.
 * quiesce/select/rollback must not start a runtime. activate/restore must verify
 * actual primary identity/readiness; cleanup removes temporary authority. */
int pd_upgrade_transaction(int store,const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                            const struct pd_upgrade_ops* ops,int recovery);
#endif
