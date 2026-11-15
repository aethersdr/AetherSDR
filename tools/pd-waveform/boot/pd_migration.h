#ifndef AETHER_PD_MIGRATION_H
#define AETHER_PD_MIGRATION_H
#include "pd_approval.h"
#include "pd_predecessor.h"
enum { PD_MIGRATION_SIZE=308 };
struct pd_migration {
    unsigned char approval[PD_APPROVAL_SIZE];
    struct pd_predecessor predecessor;
    int fresh;
    unsigned char token_hash[32],nonce[32];
    char hostname[64];
};
int pd_migration_parse(const unsigned char* bytes,size_t size,struct pd_migration* result);
int pd_migration_read(int fd,unsigned char bytes[PD_MIGRATION_SIZE],struct pd_migration* result);
int pd_migration_matches(const struct pd_migration* approved,const struct pd_predecessor* observed);
/* Internal begin operation: no removal, activation or arbitrary command path.
 * 1: copied and predecessor stopped; 0: refused before stop or recovered;
 * -1: attempted stop, recovery could not be verified. */
/* Persist the immutable plan before arming independent recovery. No stop or
 * identity transfer; caller authenticates the capsule and live predecessor. */
int pd_migration_bind(int store,const unsigned char record[PD_MIGRATION_SIZE]);
int pd_migration_copy(int store,const unsigned char record[PD_MIGRATION_SIZE]);
/* Recover only an existing matching transaction and expected public identity;
 * creates no approval journal. 1: running predecessor and matching HTTP identity
 * verified; 0: refused; -1: recovery unverified. */
int pd_migration_recover(int store,const unsigned char record[PD_MIGRATION_SIZE]);
#endif
