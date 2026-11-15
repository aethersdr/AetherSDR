#ifndef AETHER_PD_UPGRADE_ORIGIN_H
#define AETHER_PD_UPGRADE_ORIGIN_H
#include "pd_upgrade_state.h"
enum { PD_UPGRADE_ORIGIN_SIZE=8+32+PD_UPGRADE_STATE_SIZE+PD_RELEASE_PREFIX };
struct pd_upgrade_origin {
    unsigned char record[PD_UPGRADE_ORIGIN_SIZE];
    struct pd_release management;
    struct pd_upgrade_state predecessor;
    int has_predecessor;
};
/* Internal durable input, never an operator credential. The independently
 * authenticated bootstrap captures this only after inspecting actual installed
 * management and the selected predecessor. Its complete bytes must be bound
 * into the protected recovery capsule before using them to replace management. */
int pd_upgrade_origin_make(const struct pd_upgrade* next,const struct pd_upgrade_state* prior,
                            const struct pd_release* management,struct pd_upgrade_origin* origin);
/* Rechecks next-approval binding, predecessor lineage and management signature.
 * The caller must authenticate delivery/integrity of the complete record; a
 * signed header alone does not authenticate the predecessor's whole release. */
int pd_upgrade_origin_parse(const unsigned char* bytes,size_t size,const struct pd_upgrade* next,
                             struct pd_upgrade_origin* origin);
#endif
