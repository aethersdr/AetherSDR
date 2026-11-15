#ifndef AETHER_PD_TRANSACTION_H
#define AETHER_PD_TRANSACTION_H
#include "pd_migration.h"
enum pd_transaction_phase {
    PD_TRANSACTION_PREPARING=1,
    PD_TRANSACTION_COPYING=2,
    PD_TRANSACTION_RETIRING=3,
    PD_TRANSACTION_ACTIVATING=4,
    PD_TRANSACTION_COMMITTED=5,
    PD_TRANSACTION_ROLLBACK=6,
    PD_TRANSACTION_ROLLED_BACK=7
};
/* These are linked native adapters, never commands or paths from a client.
 * activate succeeds only after checking the new service's identity/readiness.
 * rollback reconciles observed resources, stops the owned new runtime and
 * preserves its latest identity before restoring the approved predecessor.
 * cleanup removes temporary installation/recovery authority, not the journal. */
struct pd_transaction_ops {
    void* context;
    int (*prepare)(void*);
    int (*copy)(void*);
    int (*retire)(void*);
    int (*activate)(void*);
    int (*rollback)(void*);
    int (*cleanup)(void*);
};
/* 1 committed, 0 rolled back, -1 recovery/cleanup pending, -2 refused.
 * Re-entry before commitment rolls back; after commitment it finishes cleanup.
 * The caller authenticates the release/live target and arms independent local
 * recovery before this function can stop or retire the predecessor. */
int pd_transaction_run(int store,const unsigned char approval[PD_MIGRATION_SIZE],
                       const struct pd_transaction_ops* ops);
/* Boot recovery never takes a forward step, even without a published journal.
 * Only rollback and cleanup callbacks are required. */
int pd_transaction_recover(int store,const unsigned char approval[PD_MIGRATION_SIZE],
                           const struct pd_transaction_ops* ops);
/* Read-only adapter gate; the transaction caller retains its exclusive lock. */
int pd_transaction_at_phase(int store,const unsigned char approval[PD_MIGRATION_SIZE],unsigned phase);
/* Return a read-only shared lock only for a committed, matching migration.
 * Keep it in the session supervisor until its child exits. No file is created. */
int pd_transaction_session_lock(int store);
/* Production entry: open only the architecture's canonical protected store. */
int pd_transaction_session_open(void);
/* Runtime preparation/readiness may also run during installation ACTIVATING
 * or upgrade ACTIVATING/RESTORING while the worker holds the exclusive lock.
 * Upgrade readiness also requires its recovery capsule. Sessions never use this exception.
 * On success, close *lease when the caller's setup finishes; -1 means the
 * worker owns serialization. Recovery must stop these units before rollback. */
int pd_transaction_runtime_gate(int store,int* lease);
int pd_transaction_runtime_open(int* lease);
#endif
