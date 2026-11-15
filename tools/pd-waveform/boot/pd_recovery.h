#ifndef AETHER_PD_RECOVERY_H
#define AETHER_PD_RECOVERY_H
#include "pd_migration.h"
enum { PD_RECOVERY_SIZE=332 };
struct pd_recovery {
    struct pd_migration migration;
    char radio_address[16];
    char hostname[42];
};
int pd_recovery_parse(const unsigned char* bytes,size_t size,struct pd_recovery* result);
/* Caller holds migration.lock and authenticates the target/release approval.
 * This immutable recipe must be durable before stopping the predecessor. */
int pd_recovery_prepare(int store,const unsigned char record[PD_MIGRATION_SIZE],
                        const struct pd_predecessor* observed);
int pd_recovery_read(int store,const unsigned char record[PD_MIGRATION_SIZE],struct pd_recovery* result);
#endif
