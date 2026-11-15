#ifndef AETHER_PD_BOOT_WAIT_H
#define AETHER_PD_BOOT_WAIT_H
#include "pd_identity.h"
/* Cold payload preparation allows at most60s for the local radio API. Socket
 * ownership uses one bounded probe. Caller retains its original runtime lease;
 * authorization is checked on both sides of each probe and sleep. */
int pd_boot_identity_wait(int store,struct pd_identity* identity,int cold);
#endif
