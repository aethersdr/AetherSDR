#ifndef AETHER_PD_UPGRADE_CAPSULE_H
#define AETHER_PD_UPGRADE_CAPSULE_H
#include "pd_upgrade.h"
#include "pd_readiness.h"
#include "pd_upgrade_origin.h"
enum { PD_UPGRADE_CAPSULE_SIZE=8+PD_UPGRADE_APPROVAL_SIZE+32+64+16+256,
       PD_UPGRADE_CAPSULE_ORIGIN_SIZE=PD_UPGRADE_CAPSULE_SIZE+PD_UPGRADE_ORIGIN_SIZE,
       PD_UPGRADE_WORKER_MAX=512*1024 };
struct pd_upgrade_capsule {
    int parent,lock,directory,worker;
    unsigned char intent[PD_UPGRADE_CAPSULE_ORIGIN_SIZE];
    size_t intent_size;
    struct pd_upgrade approval;
    struct pd_readiness expected;
    struct pd_upgrade_origin origin;
};
/* The independently authenticated bootstrap supplies its trusted worker and
 * operator-approved release/key transition after pair/target verification.
 * The legacy entry persists worker, approval and public identity: private state
 * stays in the existing store. This function neither arms units nor executes. */
int pd_upgrade_capsule_publish(int parent,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected);
/* Version2 additionally binds the authenticated prior-management record into
 * the durable intent and started receipt. Legacy version1 remains readable. */
int pd_upgrade_capsule_publish_origin(int parent,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected,const struct pd_upgrade_origin* origin);
/* Successor staging is inert until fixed recovery units are durable. Publication
 * and terminal unit cleanup share the namespace lease. The fixed boot/socket
 * service guard also excludes older cleanup already past that lease. */
int pd_upgrade_capsule_publish_armed(int parent,int units,int worker,const unsigned char key[32],
                               const unsigned char approval[PD_UPGRADE_APPROVAL_SIZE],
                               const struct pd_readiness* expected,const struct pd_upgrade_origin* origin);
/* Retains the protected namespace lease, checks the configured candidate key,
 * exact worker hash and intent. The worker must revalidate target and pair. */
int pd_upgrade_capsule_open(int parent,const unsigned char key[32],struct pd_upgrade_capsule* capsule);
int pd_upgrade_capsule_self(const struct pd_upgrade_capsule* capsule,int executable);
/* Durable before the first runtime stop. 1 first attempt, 0 restarted attempt,
 * -1 refused/incomplete. Every restarted attempt must take rollback, even when
 * it died before obtaining the exclusive runtime transaction lease. */
int pd_upgrade_capsule_begin(struct pd_upgrade_capsule* capsule);
/* Detach and remove only after a durable matching terminal upgrade journal.
 * The permanent boot entry must finish cleanup after worker removal. */
int pd_upgrade_capsule_retire(struct pd_upgrade_capsule* capsule,int store);
int pd_upgrade_capsule_cleanup_retired(int parent,int store,const unsigned char key[32]);
/* Retains the namespace lease through fixed recovery-unit removal. */
int pd_upgrade_capsule_cleanup_units(int parent,int units,int store,const unsigned char key[32]);
void pd_upgrade_capsule_close(struct pd_upgrade_capsule* capsule);
#endif
