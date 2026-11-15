#ifndef AETHER_PD_CAPSULE_H
#define AETHER_PD_CAPSULE_H
#include "pd_migration.h"
#include "pd_release.h"
enum { PD_CAPSULE_SIZE=72+PD_MIGRATION_SIZE, PD_CAPSULE_WORKER_MAX=512*1024 };
struct pd_capsule {
    int parent,lock,directory,worker,release;
    unsigned char intent[PD_CAPSULE_SIZE];
    struct pd_migration migration;
    struct pd_approval approval;
    struct pd_release artifact;
};
/* Linked installer operations, not a path/command request interface. Caller
 * authenticates live target/predecessor before publishing and supplies its own
 * trusted executable descriptor. Nothing is enabled, executed or stopped here. */
int pd_capsule_publish(int parent,int worker,int release,const unsigned char key[32],
                       const unsigned char plan[PD_MIGRATION_SIZE]);
/* Holds the root-only namespace lock until close. Reauthenticates the persisted
 * release and executable binding; caller still checks live radio identity.
 * EWOULDBLOCK identifies a busy namespace lease; other refusals are not busy. */
int pd_capsule_open(int parent,const unsigned char key[32],struct pd_capsule* capsule);
int pd_capsule_self(const struct pd_capsule* capsule,int executable);
/* A durable matching terminal transaction is required before executable
 * authority is detached. Units enter through the permanent narrow boot helper,
 * so cleanup can finish after the temporary worker has been removed. */
int pd_capsule_retire(struct pd_capsule* capsule,int store);
int pd_capsule_cleanup_retired(int parent,int store,const unsigned char key[32],
                              const unsigned char plan[PD_MIGRATION_SIZE]);
/* Finish retired-file and temporary-unit cleanup under one namespace lock.
 * Refuses while an active capsule exists. Does not start or stop a service. */
int pd_capsule_finish_cleanup(int parent,int units,int store,const unsigned char key[32],
                             const unsigned char plan[PD_MIGRATION_SIZE]);
/* Normal boot reconciles inert terminal unit residue without dispatching a
 * worker. Pending transactions are left to the runtime gate/recovery service. */
int pd_capsule_reconcile_terminal(int parent,int units,int store,const unsigned char key[32]);
void pd_capsule_close(struct pd_capsule* capsule);
#endif
