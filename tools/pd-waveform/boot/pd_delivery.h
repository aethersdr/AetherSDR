#ifndef AETHER_PD_DELIVERY_H
#define AETHER_PD_DELIVERY_H
#include <stdint.h>
enum { PD_DELIVERY_CONSENT_SIZE=80 };
/* After authenticated operator consent: protected receipt FD3 binds the exact
 * envelope and target plan. FD0 envelope, FD4 plan, FD5 candidate, FD6 previous
 * (upgrade only). run is the fixed protected /run directory, never a user path.
 * Returns 0 on successful worker exit and staging removal, 3 refusal, 4 failed
 * worker/timeout, 5 incomplete cleanup. Does not confer initial root authority. */
int pd_delivery_run(int run,const unsigned char key[32],uint32_t arch);
void pd_delivery_cancel(int signal);
#endif
