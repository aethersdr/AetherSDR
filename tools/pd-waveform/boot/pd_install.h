#ifndef AETHER_PD_INSTALL_H
#define AETHER_PD_INSTALL_H
#include "pd_release.h"
int pd_install_prepare(int parent,int units,int stage,const unsigned char key[32],
                       const struct pd_release* release,const unsigned char approved[32]);
/* Remove only matching extracted release files from the fixed RAM stage.
 * Caller retains the capsule lease; absence/partial prior cleanup is allowed. */
int pd_install_stage_discard(int run,const struct pd_release* release,const unsigned char approved[140]);
#endif
