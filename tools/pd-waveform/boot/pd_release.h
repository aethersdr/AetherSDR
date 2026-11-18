#ifndef AETHER_PD_RELEASE_H
#define AETHER_PD_RELEASE_H
#include <stddef.h>
#include <stdint.h>
enum { PD_RELEASE_COUNT=8, PD_RELEASE_HEADER=80, PD_RELEASE_RECORD=36,
       PD_RELEASE_MANIFEST=PD_RELEASE_HEADER+PD_RELEASE_COUNT*PD_RELEASE_RECORD,
       PD_RELEASE_PREFIX=PD_RELEASE_MANIFEST+64, PD_RELEASE_MAX=18*1024*1024 };
extern const char* const pd_release_names[PD_RELEASE_COUNT];
struct pd_release {
    unsigned char manifest[PD_RELEASE_MANIFEST];
    unsigned char signature[64];
    uint32_t arch,profile,boot_handoff,managed_startup,origin_recovery,generation_selection,guardian_hold,control_restart,length[PD_RELEASE_COUNT],offset[PD_RELEASE_COUNT];
};
/* Select only fixed definitions whose stop policy matches the signed manifest. */
const char* pd_release_unit_text(const struct pd_release* release,unsigned index);
/* Admission is supplied by the caller's independently checked radio identity;
 * a signed profile is not itself evidence of the connected hardware. */
int pd_release_read(int fd,const unsigned char key[32],uint32_t arch,uint32_t profile,
                    const char* firmware,const unsigned char approved_digest[32],struct pd_release* release);
int pd_release_extract(int fd,int directory,const unsigned char key[32],const struct pd_release* release);
/* Shared envelope/member validation for reopening a protected staged release.
 * Header authentication alone does not verify members or the whole approval. */
int pd_release_header_parse(const unsigned char* bytes,size_t size,const unsigned char key[32],
                            uint32_t arch,uint32_t profile,const char* firmware,struct pd_release* release);
int pd_release_record_validate(const unsigned char* bytes,size_t size,unsigned index,const struct pd_release* release);
#endif
