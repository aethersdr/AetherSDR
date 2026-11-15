#ifndef AETHER_PD_MANAGEMENT_H
#define AETHER_PD_MANAGEMENT_H
#include "pd_upgrade.h"
#include "pd_upgrade_origin.h"
struct pd_management_blob {
    const unsigned char* boot;
    size_t boot_size;
    const unsigned char* licenses;
    size_t license_size;
    unsigned char header[PD_RELEASE_PREFIX];
    unsigned char release_digest[32];
};
/* Internal first-transition primitive. The caller authenticates the bootstrap,
 * embeds its candidate management bytes, authenticates the previous release,
 * holds the exclusive transaction lease and arms independent recovery first.
 * No payload selection, state-directory access or service execution occurs. */
int pd_management_apply(int store,int units,const struct pd_upgrade* approved,
                         const struct pd_management_blob* candidate);
/* The caller decoded origin from its authenticated recovery capsule. It names
 * the retained management independently of the selected previous payload. */
int pd_management_apply_origin(int store,int units,const struct pd_upgrade* approved,
                         const struct pd_management_blob* candidate,const struct pd_upgrade_origin* origin);
#endif
