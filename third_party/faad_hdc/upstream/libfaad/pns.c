/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
**
** $Id: pns.c,v 1.39 2010/06/04 20:47:56 menno Exp $
**/

#include "common.h"
#include "structs.h"

#include "pns.h"


/* static function declarations */
static void gen_rand_vector(real_t *spec, int16_t scale_factor, uint16_t size,
                            uint8_t sub,
                            /* RNG states */ uint32_t *__r1, uint32_t *__r2);


#ifdef FIXED_POINT

static real_t const pow2_table[] =
{
    COEF_CONST(1.0),
    COEF_CONST(1.18920711500272),
    COEF_CONST(1.41421356237310),
    COEF_CONST(1.68179283050743)
};

// mean_energy_table[x] == sqrt(3 / x)
static real_t const mean_energy_table[] =
{
    COEF_CONST(0.0),                // should not happen
    COEF_CONST(1.7320508075688772),
    COEF_CONST(1.224744871391589),
    COEF_CONST(1.0),                // sqrt(3/3)
    COEF_CONST(0.8660254037844386),
    COEF_CONST(0.7745966692414834),
    COEF_CONST(0.7071067811865476),
    COEF_CONST(0.6546536707079771),
    COEF_CONST(0.6123724356957945),
    COEF_CONST(0.5773502691896257),
    COEF_CONST(0.5477225575051661),
    COEF_CONST(0.5222329678670935),
    COEF_CONST(0.5),                // sqrt(3/12)
    COEF_CONST(0.4803844614152614),
    COEF_CONST(0.4629100498862757),
    COEF_CONST(0.4472135954999579),
};
#endif

/* The function gen_rand_vector(addr, size) generates a vector of length
   <size> with signed random values of average energy MEAN_NRG per random
   value. A suitable random number generator can be realized using one
   multiplication/accumulation per random value.
*/
static INLINE void gen_rand_vector(real_t *spec, int16_t scale_factor, uint16_t size,
                                   uint8_t sub,
                                   /* RNG states */ uint32_t *__r1, uint32_t *__r2)
{
#ifndef FIXED_POINT
    uint16_t i;
    real_t energy = 0.0;
    (void)sub;

    scale_factor = min(max(scale_factor, -120), 120);

    for (i = 0; i < size; i++)
    {
        real_t tmp = (real_t)(int32_t)ne_rng(__r1, __r2);
        spec[i] = tmp;
        energy += tmp*tmp;
    }

    if (energy > 0)
    {
        real_t scale = (real_t)1.0/(real_t)sqrt(energy);
        scale *= (real_t)pow(2.0, 0.25 * scale_factor);
        for (i = 0; i < size; i++)
        {
            spec[i] *= scale;
        }
    }
#else
    uint16_t i;
    real_t scale;
    int32_t exp, frac;
    int32_t idx, mask;

    /* IMDCT pre-scaling */
    scale_factor -= 4 * sub;

    // 52 stands for 2**13 == 8192 factor; larger factor causes overflows later (in cfft).
    scale_factor = min(max(scale_factor, -(REAL_BITS * 4)), 52);

    exp = scale_factor >> 2;
    frac = scale_factor & 3;

    /* 29 <= REAL_BITS + exp <= 0 */
    mask = (1 << (REAL_BITS + exp)) - 1;

    idx = size;
    scale = COEF_CONST(1);
    // At most 2 iterations.
    while (idx >= 16)
    {
        idx >>= 2;
        scale >>= 1;
    }
    scale = MUL_C(scale, mean_energy_table[idx]);
    if (frac)
        scale = MUL_C(scale, pow2_table[frac]);
    // scale is less than 4.0 now.

    for (i = 0; i < size; i++)
    {
        real_t tmp = (int32_t)ne_rng(__r1, __r2);
        if (tmp < 0)
            tmp = -(tmp & mask);
        else
            tmp = (tmp & mask);
        spec[i] = MUL_C(tmp, scale);
    }
#endif
}

void pns_decode(ic_stream *ics_left, ic_stream *ics_right,
                real_t *spec_left, real_t *spec_right, uint16_t frame_len,
                uint8_t channel_pair, uint8_t object_type,
                /* RNG states */ uint32_t *__r1, uint32_t *__r2)
{
    uint8_t g, sfb, b;
    uint16_t begin, end;

    uint8_t group = 0;
    uint16_t nshort = frame_len >> 3;

    uint8_t sub = 0;

#ifdef FIXED_POINT
    /* IMDCT scaling */
    if (object_type == LD)
    {
        sub = 9 /*9*/;
    } else {
        if (ics_left->window_sequence == EIGHT_SHORT_SEQUENCE)
            sub = 7 /*7*/;
        else
            sub = 10 /*10*/;
    }
#else
    (void)object_type;
#endif

    for (g = 0; g < ics_left->num_window_groups; g++)
    {
        /* Do perceptual noise substitution decoding */
        for (b = 0; b < ics_left->window_group_length[g]; b++)
        {
            uint16_t base = group * nshort;
            for (sfb = 0; sfb < ics_left->max_sfb; sfb++)
            {
                uint32_t r1_dep = 0, r2_dep = 0;

                if (is_noise(ics_left, g, sfb))
                {
#ifdef LTP_DEC
                    /* Simultaneous use of LTP and PNS is not prevented in the
                       syntax. If both LTP, and PNS are enabled on the same
                       scalefactor band, PNS takes precedence, and no prediction
                       is applied to this band.
                    */
                    ics_left->ltp.long_used[sfb] = 0;
                    ics_left->ltp2.long_used[sfb] = 0;
#endif

#ifdef MAIN_DEC
                    /* For scalefactor bands coded using PNS the corresponding
                       predictors are switched to "off".
                    */
                    ics_left->pred.prediction_used[sfb] = 0;
#endif
                    begin = min(base + ics_left->swb_offset[sfb], ics_left->swb_offset_max);
                    end = min(base + ics_left->swb_offset[sfb+1], ics_left->swb_offset_max);

                    r1_dep = *__r1;
                    r2_dep = *__r2;

                    /* Generate random vector */
                    gen_rand_vector(&spec_left[begin],
                        ics_left->scale_factors[g][sfb], end - begin, sub, __r1, __r2);
                }

/* From the spec:
   If the same scalefactor band and group is coded by perceptual noise
   substitution in both channels of a channel pair, the correlation of
   the noise signal can be controlled by means of the ms_used field: While
   the default noise generation process works independently for each channel
   (separate generation of random vectors), the same random vector is used
   for both channels if ms_used[] is set for a particular scalefactor band
   and group. In this case, no M/S stereo coding is carried out (because M/S
   stereo coding and noise substitution coding are mutually exclusive).
   If the same scalefactor band and group is coded by perceptual noise
   substitution in only one channel of a channel pair the setting of ms_used[]
   is not evaluated.
*/
                if ((ics_right != NULL)
                    && is_noise(ics_right, g, sfb))
                {
#ifdef LTP_DEC
                    /* See comment above. */
                    ics_right->ltp.long_used[sfb] = 0;
                    ics_right->ltp2.long_used[sfb] = 0;
#endif
#ifdef MAIN_DEC
                    /* See comment above. */
                    ics_right->pred.prediction_used[sfb] = 0;
#endif

                    if (channel_pair && is_noise(ics_left, g, sfb) &&
                        (((ics_left->ms_mask_present == 1) &&
                        (ics_left->ms_used[g][sfb])) ||
                        (ics_left->ms_mask_present == 2)))
                    {
                        /*uint16_t c;*/

                        begin = min(base + ics_right->swb_offset[sfb], ics_right->swb_offset_max);
                        end = min(base + ics_right->swb_offset[sfb+1], ics_right->swb_offset_max);

                        /* Generate random vector dependent on left channel*/
                        gen_rand_vector(&spec_right[begin],
                            ics_right->scale_factors[g][sfb], end - begin, sub, &r1_dep, &r2_dep);

                    } else /*if (ics_left->ms_mask_present == 0)*/ {
                        begin = min(base + ics_right->swb_offset[sfb], ics_right->swb_offset_max);
                        end = min(base + ics_right->swb_offset[sfb+1], ics_right->swb_offset_max);

                        /* Generate random vector */
                        gen_rand_vector(&spec_right[begin],
                            ics_right->scale_factors[g][sfb], end - begin, sub, __r1, __r2);
                    }
                }
            } /* sfb */
            group++;
        } /* b */
    } /* g */
}
