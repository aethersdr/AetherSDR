#ifndef AETHER_PD_BUNDLE_H
#define AETHER_PD_BUNDLE_H
#include <stddef.h>
#include <stdint.h>

enum { PD_BUNDLE_COUNT=11, PD_BUNDLE_HEADER=48, PD_BUNDLE_RECORD=72,
       PD_BUNDLE_MANIFEST=PD_BUNDLE_HEADER+PD_BUNDLE_COUNT*PD_BUNDLE_RECORD,
       PD_BUNDLE_PREFIX=PD_BUNDLE_MANIFEST+64,
       PD_BUNDLE_MAX=16*1024*1024, PD_BUNDLE_EXPANDED_MAX=48*1024*1024 };
extern const char* const pd_bundle_names[PD_BUNDLE_COUNT];
struct pd_bundle {
    unsigned char manifest[PD_BUNDLE_MANIFEST];
    uint32_t arch;
    uint32_t compressed[PD_BUNDLE_COUNT];
    uint32_t expanded[PD_BUNDLE_COUNT];
    uint32_t offset[PD_BUNDLE_COUNT];
};
/* Descriptors are owned by the fixed boot caller. No paths from the bundle
 * are interpreted. Verification and extraction never execute a payload. */
int pd_bundle_read(int fd,const unsigned char key[32],uint32_t arch,struct pd_bundle* bundle);
int pd_bundle_hash(int fd,unsigned char digest[32]);
int pd_bundle_extract(int fd,int directory,const struct pd_bundle* bundle);
int pd_bundle_validate(int fd,const struct pd_bundle* bundle);
int pd_bundle_matches(int directory,const struct pd_bundle* bundle);
#endif
