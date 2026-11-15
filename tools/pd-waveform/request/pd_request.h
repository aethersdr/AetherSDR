#ifndef AETHER_PD_REQUEST_H
#define AETHER_PD_REQUEST_H
/* Fixed unprivileged entry: private unlinked RAM configuration on fd3 and
 * readiness pipe on fd4. No paths, commands or privilege requests are accepted. */
int pd_request_inherited(void);
#endif
