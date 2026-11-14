/*
 * osd174_91 -- ordered-statistics decoder for the (174,91) FT8 code.
 *
 * C port of WSJT-X's lib/ft8/osd174_91.f90 by Steve Franke K9AN and
 * Joe Taylor K1JT.  See PROVENANCE.md in this directory for the upstream
 * revision, the licence, and what was changed in the port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ⚠ This file is GPL-3.0, unlike the MIT-licensed ft8_lib it is called
 * from.  That is why it lives in its own directory rather than inside
 * ft8_lib/: the licence boundary is meant to be visible in the layout.
 * AetherSDR is GPL-3.0-or-later, so the combination is fine.
 */
#ifndef AETHER_OSD174_91_H
#define AETHER_OSD174_91_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Depth of the OSD search.  Higher finds more and costs more.
 *
 * WSJT-X calls FT8 with norder=2, which is OSD_DEPTH_2 here.  The on-radio
 * agent may use a lower depth to fit the receive budget on a Cortex-A8;
 * that is a measured tradeoff, not a guess -- see PROVENANCE.md. */
typedef enum
{
    /* ⚠ Order 0 only -- NOT "off". It still runs the Gaussian elimination
     * and re-encodes the hard decisions, which recovers frames belief
     * propagation missed. The caller treats this value as "do not call
     * OSD at all", which is what makes a true control arm; passing it
     * here does order-0 decoding. */
    OSD_DEPTH_OFF = 0,
    OSD_DEPTH_1 = 1,   /* nord=1, no preprocessing */
    OSD_DEPTH_2 = 2,   /* nord=1, rule 1 */
    OSD_DEPTH_3 = 3,   /* nord=1, rules 1+2 (needs the box tables) */
    OSD_DEPTH_4 = 4,   /* nord=2, rules 1+2 -- what WSJT-X uses for FT8 */
    OSD_DEPTH_5 = 5,   /* nord=3, rules 1+2 */
    OSD_DEPTH_6 = 6    /* nord=4, rules 1+2 */
} osd_depth_t;

/*
 * Run ordered-statistics decoding on a 174-bit soft-decision codeword.
 *
 *   llr        [174] log-likelihood ratios, same convention as ft8_lib's
 *              log174: positive means the bit is probably 1.
 *   k          number of message bits used, in [77, 91].  77 uses no CRC
 *              bits for detection; 91 uses all 14.  WSJT-X uses 91 here.
 *   depth      how hard to search (see above).
 *   out_msg91  [91] decoded message bits on success.
 *   out_nhard  if non-NULL, receives the Hamming distance between the
 *              chosen codeword and the hard decisions.  NEGATIVE when the
 *              CRC failed, matching the Fortran's sign convention.
 *
 * Returns 1 when a codeword was found whose CRC checks, 0 otherwise.
 *
 * Thread safety: the generator matrix is built once and cached, guarded so
 * that two threads racing the first call cannot both build it.  Everything
 * else is on the stack, so concurrent calls after that are safe.
 */
int osd174_91_decode(const float llr[174], int k, osd_depth_t depth,
                     uint8_t out_msg91[91], int* out_nhard);

#ifdef __cplusplus
}
#endif

#endif /* AETHER_OSD174_91_H */
