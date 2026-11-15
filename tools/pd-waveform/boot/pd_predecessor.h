#ifndef AETHER_PD_PREDECESSOR_H
#define AETHER_PD_PREDECESSOR_H
#include <stddef.h>
#include <sys/types.h>
struct pd_predecessor {
    char container[65],image[65];
    char radio_address[16];
    char hostname[65];
    pid_t pid;
    unsigned char executable[32];
};
/* Read-only admission of the fixed FLEX-8000 legacy shim. This does not
 * authorize removal: the caller must bind the observation to operator consent,
 * revalidate it before mutation and preserve identity/recovery first. */
int pd_predecessor_parse(const unsigned char* bytes,size_t size,struct pd_predecessor* result);
int pd_predecessor_probe(struct pd_predecessor* result);
/* Absence is distinct from a failed inventory/identity check. */
int pd_predecessor_absent(void);
int pd_predecessor_parse_status(const unsigned char* bytes,size_t size,struct pd_predecessor* result);
struct pd_recovery;
/* Metadata-only admission for the fixed single-bind rollback layout.
 * The caller must verify executable identity before starting or adopting it. */
int pd_predecessor_parse_restored(const unsigned char* bytes,size_t size,
                                 const struct pd_recovery* recipe,struct pd_predecessor* result);
int pd_predecessor_restored_status(const struct pd_recovery* recipe,struct pd_predecessor* result);
int pd_predecessor_format(int restored,char* bytes,size_t capacity);
/* Installer-internal fixed operations. The caller authenticates the release,
 * target and predecessor approval before calling these mutation functions. */
int pd_predecessor_pin(const struct pd_predecessor* expected,int* root,int* state,int* tsnet);
int pd_predecessor_stop(const struct pd_predecessor* expected,int root);
int pd_predecessor_stopped(const struct pd_predecessor* expected);
int pd_predecessor_recover(const struct pd_predecessor* expected,int root);
/* Re-admit a stopped predecessor without inherited filesystem descriptors. */
int pd_predecessor_recover_fresh(const struct pd_predecessor* expected);
int pd_predecessor_mount_path(const unsigned char* bytes,size_t size,char layer[65]);
/* The approved 0.5.2 image has one immutable overlay layer. Verification only
 * reads that layer; it does not mount, create, start or remove anything. */
int pd_predecessor_image_parse(const unsigned char* bytes,size_t size,const struct pd_predecessor* expected,char layer[65]);
const char* pd_predecessor_image_format(void);
int pd_predecessor_image_verify(const struct pd_predecessor* expected);
/* Verify the exact container's direct upper/lower image layers without mounts. */
int pd_predecessor_overlay_verify(const struct pd_predecessor* expected);
/* Linked transaction adapters, not request interfaces. The caller authenticates
 * release/target and holds transaction.lock. Both require the canonical store,
 * matching durable phase/recipe and no new AetherPD runtime. Restore returns
 * a verified running executable, not HTTP/Tailscale readiness. */
int pd_predecessor_retire(int store,const unsigned char* approval);
int pd_predecessor_restore(int store,const unsigned char* approval);
#endif
