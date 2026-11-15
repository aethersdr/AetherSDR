#ifndef AETHER_PD_BOOTSTRAP_ARTIFACT_H
#define AETHER_PD_BOOTSTRAP_ARTIFACT_H
#include <stddef.h>
#include <stdint.h>

enum { PD_BOOTSTRAP_INSTALL=1, PD_BOOTSTRAP_UPGRADE=2,
       PD_BOOTSTRAP_HEADER=120, PD_BOOTSTRAP_PREFIX=184,
       PD_BOOTSTRAP_MAX=512*1024 };
struct pd_bootstrap_artifact {
    unsigned char* executable;
    size_t size;
    unsigned char digest[32];
};
/* Returns an owned verified snapshot, never a path to reopen. The caller supplies
 * an independently trusted key and the release/source/role selected for review.
 * This authenticates distribution, not operator consent or live-target admission.
 * No executable is written or launched here. */
int pd_bootstrap_artifact_read(int fd,const unsigned char key[32],uint32_t arch,uint32_t role,
    const unsigned char source[32],const unsigned char release[32],struct pd_bootstrap_artifact* artifact);
void pd_bootstrap_artifact_close(struct pd_bootstrap_artifact* artifact);
#endif
