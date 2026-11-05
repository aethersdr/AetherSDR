/* wsprd.c calls the Fortran ordered-statistics decoder (osdwspr.f90) only
 * when run with -o. The agent never passes -o, so this stands in for it and
 * the Fortran runtime is not needed. Part of AetherSDR, GPL-3.0-or-later. */
#include <stdlib.h>

void osdwspr_(float ss[], unsigned char apmask[], int *ndeep, unsigned char cw[], int *nhardmin, float *dmin)
{
    (void)ss; (void)apmask; (void)ndeep; (void)cw; (void)nhardmin; (void)dmin;
    abort();
}
