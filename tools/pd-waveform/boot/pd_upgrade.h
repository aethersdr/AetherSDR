#ifndef AETHER_PD_UPGRADE_H
#define AETHER_PD_UPGRADE_H
#include "pd_approval.h"
#include "pd_identity.h"
#include "pd_release.h"
enum { PD_UPGRADE_APPROVAL_SIZE=8+2*PD_APPROVAL_SIZE+64 };
struct pd_upgrade {
    unsigned char record[PD_UPGRADE_APPROVAL_SIZE];
    struct pd_approval previous,candidate;
    unsigned char previous_key[32],candidate_key[32];
    struct pd_release previous_release,candidate_release;
};
struct pd_upgrade_budget {
    uint64_t required_bytes,available_bytes,required_inodes,available_inodes;
};
/* Read-only staging admission after authenticating inputs and existing prefixes.
 * Includes both releases, allocation rounding, directory growth and the store
 * reserve. Other future upgrade allocations require separate worker accounting. */
int pd_upgrade_space(int stage,const struct pd_release* previous,const struct pd_release* candidate,
                      struct pd_upgrade_budget* budget);
/* Protected operator approval binds both complete releases and both trust keys.
 * The record grants no privilege and its keys are not self-authenticating. */
int pd_upgrade_parse(const unsigned char* bytes,size_t size,struct pd_upgrade* upgrade);
/* Read-only preflight against a committed installation or terminal upgrade.
 * A successor must name the selected predecessor release/key; its staged pair,
 * selector and retained candidate management must agree with the terminal record.
 * The selected predecessor remains complete. A protected terminal receipt can
 * authenticate retained management after unselected-payload compaction; without
 * that receipt both releases must remain intact. No stage rotates.
 * expected_key belongs to the independently
 * authenticated bootstrap, never to an untrusted request. The caller supplies
 * a freshly observed radio identity and rechecks it before any live mutation. */
int pd_upgrade_inspect(int store,int units,int approval,int previous,int candidate,
                       const unsigned char expected_key[32],uint32_t arch,
                       const struct pd_identity* identity,struct pd_upgrade* upgrade);
struct pd_upgrade_origin;
/* Same inspection, retaining authenticated management provenance for the next
 * recovery capsule. Caller holds the store/bootstrap lease throughout capture
 * and publication; this function creates no files and grants no authority. */
int pd_upgrade_inspect_origin(int store,int units,int approval,int previous,int candidate,
                       const unsigned char expected_key[32],uint32_t arch,
                       const struct pd_identity* identity,struct pd_upgrade* upgrade,struct pd_upgrade_origin* origin);
/* Stage only into a new protected private directory supplied by the fixed
 * worker. All eight records of each release, including the inner payload, are
 * revalidated during copying. Failure leaves an uncommitted stage for recovery;
 * it never selects a release, executes code or modifies installed state. */
int pd_upgrade_stage(int stage,int previous,int candidate,const struct pd_upgrade* upgrade);
/* Fresh-process recovery from staged bytes only. Requires exact protected
 * inventory, both signatures and whole approvals, every member and both inner
 * payloads. Syncs the verified files/directories before reporting success.
 * The fixed worker must serialize stage access and recheck the live target. */
int pd_upgrade_reopen(int stage,const unsigned char expected_key[32],uint32_t arch,
                      const struct pd_identity* identity,struct pd_upgrade* upgrade);
/* Verify only the selected side (0 previous, 1 candidate), retaining approval,
 * target and key binding. Recovery must not depend on an intact rejected side.
 * The other side's release structure is zero, not authenticated by this call. */
int pd_upgrade_reopen_choice(int stage,const unsigned char expected_key[32],uint32_t arch,
                             const struct pd_identity* identity,unsigned choice,struct pd_upgrade* upgrade);
/* Before activation only: resume matching prefixes using bounded authenticated
 * private input snapshots. Validate the entire existing stage before writing;
 * never truncate, delete or replace a foreign object. Caller serializes access
 * and performs the committed-installation preflight again before invoking. */
int pd_upgrade_resume(int stage,int previous,int candidate,const unsigned char expected_key[32],uint32_t arch,
                      const struct pd_identity* identity,const struct pd_upgrade* approved);
#endif
