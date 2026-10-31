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
** $Id: huffman.c,v 1.26 2007/11/01 12:33:30 menno Exp $
**/

#include "common.h"
#include "structs.h"

#include <stdlib.h>
#ifdef ANALYSIS
#include <stdio.h>
#endif

#include "bits.h"
#include "huffman.h"
#include "codebook/hcb.h"


/* static function declarations */
static INLINE void huffman_sign_bits(bitfile *ld, int16_t *sp, uint8_t len);
static INLINE uint8_t huffman_getescape(bitfile *ld, int16_t *sp);
static uint8_t huffman_2step_quad(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_2step_quad_sign(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_2step_pair(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_2step_pair_sign(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_binary_quad(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_binary_quad_sign(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_binary_pair(uint8_t cb, bitfile *ld, int16_t *sp);
static uint8_t huffman_binary_pair_sign(uint8_t cb, bitfile *ld, int16_t *sp);
#if 0
static int16_t huffman_codebook(uint8_t i);
#endif
static void vcb11_check_LAV(uint8_t cb, int16_t *sp);

int8_t huffman_scale_factor(bitfile *ld)
{
    uint16_t offset = 0;

    while (hcb_sf[offset][1])
    {
        uint8_t b = faad_get1bit(ld
            DEBUGVAR(1,255,"huffman_scale_factor()"));
        offset += hcb_sf[offset][b];
    }

    return hcb_sf[offset][0];
}


static const uint8_t hcbN[LAST_CB_IDX + 1] =
{   0,      5,      5,    0,      5,    0,      5,    0,      5,    0,       6,       5};
static const hcb* hcb_table[LAST_CB_IDX + 1] =
{NULL, hcb1_1, hcb2_1, NULL, hcb4_1, NULL, hcb6_1, NULL, hcb8_1, NULL, hcb10_1, hcb11_1};
static const hcb_2_quad* hcb_2_quad_table[LAST_CB_IDX + 1] =
{NULL, hcb1_2, hcb2_2, NULL, hcb4_2, NULL,   NULL, NULL,   NULL, NULL,    NULL,    NULL};
static const hcb_2_pair* hcb_2_pair_table[LAST_CB_IDX + 1] =
{NULL,   NULL,   NULL, NULL,   NULL, NULL, hcb6_2, NULL, hcb8_2, NULL, hcb10_2, hcb11_2};
static const hcb_bin_pair* hcb_bin_table[LAST_CB_IDX + 1] =
{NULL,   NULL,   NULL, NULL,   NULL, hcb5,   NULL, hcb7,   NULL, hcb9,    NULL,    NULL};
/*                     hcb3 is the unique case */

/* defines whether a huffman codebook is unsigned or not */
/* Table 4.6.2 */
static uint8_t unsigned_cb[32] = { 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0,
           /* codebook 16 to 31 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

static INLINE void huffman_sign_bits(bitfile *ld, int16_t *sp, uint8_t len)
{
    uint8_t i;

    for (i = 0; i < len; i++)
    {
        if(sp[i])
        {
            if(faad_get1bit(ld
                DEBUGVAR(1,5,"huffman_sign_bits(): sign bit")) & 1)
            {
                sp[i] = -sp[i];
            }
        }
    }
}

static INLINE uint8_t huffman_getescape(bitfile *ld, int16_t *sp)
{
    uint8_t neg, i;
    int16_t j;
	int16_t off;
    int16_t x = *sp;

    if (x < 0)
    {
        if (x != -16)
            return 0;
        neg = 1;
    } else {
        if (x != 16)
            return 0;
        neg = 0;
    }

    for (i = 4; i < 16; i++)
    {
        if (faad_get1bit(ld
            DEBUGVAR(1,6,"huffman_getescape(): escape size")) == 0)
        {
            break;
        }
    }
    if (i >= 16)
        return 10;

    off = (int16_t)faad_getbits(ld, i
        DEBUGVAR(1,9,"huffman_getescape(): escape"));

    j = off | (1<<i);
    if (neg)
        j = -j;

    *sp = j;
    return 0;
}

static uint8_t huffman_2step_quad(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint32_t cw;
    uint16_t offset;
    uint8_t extra_bits;
    const hcb* root;
    uint8_t root_bits;
    const hcb_2_quad* table;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (hcbN[cb] == 0) __builtin_trap();
    if (hcb_table[cb] == NULL) __builtin_trap();
    if (hcb_2_quad_table[cb] == NULL) __builtin_trap();
    // In other words, `cb` is one of [1, 2, 4].
#endif
    root = hcb_table[cb];
    root_bits = hcbN[cb];
    table = hcb_2_quad_table[cb];

    cw = faad_showbits(ld, root_bits);
    offset = root[cw].offset;
    extra_bits = root[cw].extra_bits;

    if (extra_bits)
    {
        /* We know for sure it's more than `root_bits` bits long. */
        faad_flushbits(ld, root_bits);
        offset += (uint16_t)faad_showbits(ld, extra_bits);
        faad_flushbits(ld, table[offset].bits - root_bits);
    } else {
        faad_flushbits(ld, table[offset].bits);
    }

    sp[0] = table[offset].x;
    sp[1] = table[offset].y;
    sp[2] = table[offset].v;
    sp[3] = table[offset].w;

    return 0;
}

static uint8_t huffman_2step_quad_sign(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint8_t err = huffman_2step_quad(cb, ld, sp);
    huffman_sign_bits(ld, sp, QUAD_LEN);

    return err;
}

static uint8_t huffman_2step_pair(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint32_t cw;
    uint16_t offset;
    uint8_t extra_bits;
    const hcb* root;
    uint8_t root_bits;
    const hcb_2_pair* table;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (hcbN[cb] == 0) __builtin_trap();
    if (hcb_table[cb] == NULL) __builtin_trap();
    if (hcb_2_pair_table[cb] == NULL) __builtin_trap();
    // In other words, `cb` is one of [6, 8, 10, 11].
#endif
    root = hcb_table[cb];
    root_bits = hcbN[cb];
    table = hcb_2_pair_table[cb];

    cw = faad_showbits(ld, root_bits);
    offset = root[cw].offset;
    extra_bits = root[cw].extra_bits;

    if (extra_bits)
    {
        /* we know for sure it's more than hcbN[cb] bits long */
        faad_flushbits(ld, root_bits);
        offset += (uint16_t)faad_showbits(ld, extra_bits);
        faad_flushbits(ld, table[offset].bits - root_bits);
    } else {
        faad_flushbits(ld, table[offset].bits);
    }

    sp[0] = table[offset].x;
    sp[1] = table[offset].y;

    return 0;
}

static uint8_t huffman_2step_pair_sign(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint8_t err = huffman_2step_pair(cb, ld, sp);
    huffman_sign_bits(ld, sp, PAIR_LEN);

    return err;
}

static uint8_t huffman_binary_quad(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint16_t offset = 0;
    const hcb_bin_quad* table = hcb3;

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (cb != 3) __builtin_trap();
#endif

    while (!table[offset].is_leaf)
    {
        uint8_t b = faad_get1bit(ld
            DEBUGVAR(1,255,"huffman_spectral_data():3"));
        offset += table[offset].data[b];
    }

    sp[0] = table[offset].data[0];
    sp[1] = table[offset].data[1];
    sp[2] = table[offset].data[2];
    sp[3] = table[offset].data[3];

    return 0;
}

static uint8_t huffman_binary_quad_sign(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint8_t err = huffman_binary_quad(cb, ld, sp);
    huffman_sign_bits(ld, sp, QUAD_LEN);

    return err;
}

static uint8_t huffman_binary_pair(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint16_t offset = 0;
    const hcb_bin_pair* table;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (hcb_bin_table[cb] == NULL) __builtin_trap();
    if (cb == 3) __builtin_trap();
    // In other words, `cb` is one of [5, 7, 9].
#endif
    table = hcb_bin_table[cb];

    while (!table[offset].is_leaf)
    {
        uint8_t b = faad_get1bit(ld
            DEBUGVAR(1,255,"huffman_spectral_data():9"));
        offset += table[offset].data[b];
    }

    sp[0] = table[offset].data[0];
    sp[1] = table[offset].data[1];

    return 0;
}

static uint8_t huffman_binary_pair_sign(uint8_t cb, bitfile *ld, int16_t *sp)
{
    uint8_t err = huffman_binary_pair(cb, ld, sp);
    huffman_sign_bits(ld, sp, PAIR_LEN);

    return err;
}

#if 0
static int16_t huffman_codebook(uint8_t i)
{
    static const uint32_t data = 16428320;
    if (i == 0) return (int16_t)(data >> 16) & 0xFFFF;
    else        return (int16_t)data & 0xFFFF;
}
#endif

static void vcb11_check_LAV(uint8_t cb, int16_t *sp)
{
    static const uint16_t vcb11_LAV_tab[] = {
        16, 31, 47, 63, 95, 127, 159, 191, 223,
        255, 319, 383, 511, 767, 1023, 2047
    };
    uint16_t max = 0;

    if (cb < 16 || cb > 31)
        return;

    max = vcb11_LAV_tab[cb - 16];

    if ((abs(sp[0]) > max) || (abs(sp[1]) > max))
    {
        sp[0] = 0;
        sp[1] = 0;
    }
}

uint8_t huffman_spectral_data(uint8_t cb, bitfile *ld, int16_t *sp)
{
    switch (cb)
    {
    case 1: /* 2-step method for data quadruples */
    case 2:
        return huffman_2step_quad(cb, ld, sp);
    case 3: /* binary search for data quadruples */
        return huffman_binary_quad_sign(cb, ld, sp);
    case 4: /* 2-step method for data quadruples */
        return huffman_2step_quad_sign(cb, ld, sp);
    case 5: /* binary search for data pairs */
        return huffman_binary_pair(cb, ld, sp);
    case 6: /* 2-step method for data pairs */
        return huffman_2step_pair(cb, ld, sp);
    case 7: /* binary search for data pairs */
    case 9:
        return huffman_binary_pair_sign(cb, ld, sp);
    case 8: /* 2-step method for data pairs */
    case 10:
        return huffman_2step_pair_sign(cb, ld, sp);
    /* Codebook 12 is disallowed, see `section_data` */
#if 0
    case 12: {
        uint8_t err = huffman_2step_pair(11, ld, sp);
        sp[0] = huffman_codebook(0); sp[1] = huffman_codebook(1);
        return err; }
#endif
    case 11:
    {
        uint8_t err = huffman_2step_pair_sign(11, ld, sp);
        if (!err)
            err = huffman_getescape(ld, &sp[0]);
        if (!err)
            err = huffman_getescape(ld, &sp[1]);
        return err;
    }
#ifdef ERROR_RESILIENCE
    /* VCB11 uses codebook 11 */
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
    {
        uint8_t err = huffman_2step_pair_sign(11, ld, sp);
        if (!err)
            err = huffman_getescape(ld, &sp[0]);
        if (!err)
            err = huffman_getescape(ld, &sp[1]);

        /* check LAV (Largest Absolute Value) */
        /* this finds errors in the ESCAPE signal */
        vcb11_check_LAV(cb, sp);

        return err;
    }
#endif
    default:
        /* Non existent codebook number, something went wrong */
        return 11;
    }

    /* return 0; */
}


#ifdef ERROR_RESILIENCE

/* Special version of huffman_spectral_data
Will not read from a bitfile but a bits_t structure.
Will keep track of the bits decoded and return the number of bits remaining.
Do not read more than ld->len, return -1 if codeword would be longer */

int8_t huffman_spectral_data_2(uint8_t cb, bits_t *ld, int16_t *sp)
{
    uint32_t cw;
    uint16_t offset = 0;
    uint8_t extra_bits;
    uint8_t vcb11 = 0;


    switch (cb)
    {
    case 1: /* 2-step method for data quadruples */
    case 2:
    case 4: {
        const hcb* root;
        uint8_t root_bits;
        const hcb_2_quad* table;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (hcbN[cb] == 0) __builtin_trap();
        if (hcb_table[cb] == NULL) __builtin_trap();
        if (hcb_2_quad_table[cb] == NULL) __builtin_trap();
        // In other words, `cb` is one of [1, 2, 4].
#endif
        root = hcb_table[cb];
        root_bits = hcbN[cb];
        table = hcb_2_quad_table[cb];

        cw = showbits_hcr(ld, root_bits);
        offset = root[cw].offset;
        extra_bits = root[cw].extra_bits;

        if (extra_bits)
        {
            /* We know for sure it's more than root_bits bits long. */
            if (flushbits_hcr(ld, root_bits)) return -1;
            offset += (uint16_t)showbits_hcr(ld, extra_bits);
            if (flushbits_hcr(ld, table[offset].bits - root_bits)) return -1;
        } else {
            if (flushbits_hcr(ld, table[offset].bits)) return -1;
        }

        sp[0] = table[offset].x;
        sp[1] = table[offset].y;
        sp[2] = table[offset].v;
        sp[3] = table[offset].w;
        break;
    }
    case 6: /* 2-step method for data pairs */
    case 8:
    case 10:
    case 11:
    /* VCB11 uses codebook 11 */
    case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: {
        const hcb* root;
        uint8_t root_bits;
        const hcb_2_pair* table;

        if (cb >= 16)
        {
            /* store the virtual codebook */
            vcb11 = cb;
            cb = 11;
        }
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (hcbN[cb] == 0) __builtin_trap();
        if (hcb_table[cb] == NULL) __builtin_trap();
        if (hcb_2_pair_table[cb] == NULL) __builtin_trap();
        // In other words, `cb` is one of [6, 8, 10, 11].
#endif
        root = hcb_table[cb];
        root_bits = hcbN[cb];
        table = hcb_2_pair_table[cb];

        cw = showbits_hcr(ld, root_bits);
        offset = root[cw].offset;
        extra_bits = root[cw].extra_bits;

        if (extra_bits)
        {
            /* we know for sure it's more than hcbN[cb] bits long */
            if (flushbits_hcr(ld, root_bits)) return -1;
            offset += (uint16_t)showbits_hcr(ld, extra_bits);
            if (flushbits_hcr(ld, table[offset].bits - root_bits)) return -1;
        } else {
            if ( flushbits_hcr(ld, table[offset].bits)) return -1;
        }
        sp[0] = table[offset].x;
        sp[1] = table[offset].y;
        break;
    }
    case 3: { /* binary search for data quadruples */
        const hcb_bin_quad* table = hcb3;
        while (!table[offset].is_leaf)
        {
            uint8_t b;
            if (get1bit_hcr(ld, &b)) return -1;
            offset += table[offset].data[b];
        }

        sp[0] = table[offset].data[0];
        sp[1] = table[offset].data[1];
        sp[2] = table[offset].data[2];
        sp[3] = table[offset].data[3];

        break;
    }

    case 5: /* binary search for data pairs */
    case 7:
    case 9: {
        const hcb_bin_pair* table = hcb_bin_table[cb];
        while (!table[offset].is_leaf)
        {
            uint8_t b;

            if (get1bit_hcr(ld, &b) ) return -1;
            offset += table[offset].data[b];
        }

        sp[0] = table[offset].data[0];
        sp[1] = table[offset].data[1];

        break;
    }}

	/* decode sign bits */
    if (unsigned_cb[cb])
    {
        uint8_t i;
        for(i = 0; i < ((cb < FIRST_PAIR_HCB) ? QUAD_LEN : PAIR_LEN); i++)
        {
            if(sp[i])
            {
            	uint8_t b;
                if ( get1bit_hcr(ld, &b) ) return -1;
                if (b != 0) {
                    sp[i] = -sp[i];
                }
           }
        }
    }

    /* decode huffman escape bits */
    if ((cb == ESC_HCB) || (cb >= 16))
    {
        uint8_t k;
        for (k = 0; k < 2; k++)
        {
            if ((sp[k] == 16) || (sp[k] == -16))
            {
                uint8_t neg, i;
                int32_t j;
                uint32_t off;

                neg = (sp[k] < 0) ? 1 : 0;

                for (i = 4; ; i++)
                {
                    uint8_t b;
                    if (get1bit_hcr(ld, &b))
                        return -1;
                    if (b == 0)
                        break;
                }

                if (i > 32)
                    return -1;

                if (getbits_hcr(ld, i, &off))
                    return -1;
                j = off + (1<<i);
                sp[k] = (int16_t)((neg) ? -j : j);
            }
        }

        if (vcb11 != 0)
        {
            /* check LAV (Largest Absolute Value) */
            /* this finds errors in the ESCAPE signal */
            vcb11_check_LAV(vcb11, sp);
        }
    }
    return ld->len;
}

#endif

