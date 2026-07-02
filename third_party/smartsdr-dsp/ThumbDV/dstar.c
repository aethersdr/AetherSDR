///*!   \file dstar.c
// *
// *    Handles all DSTAR states
// *
// *    \date 02-JUN-2015
// *    \author Ed Gonzalez KG5FBT modified from original in OpenDV code (C) 2009 Jonathan Naylor, G4KLX
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2012-2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "DStarDefines.h"
#include "gmsk_modem.h"
#include "bit_pattern_matcher.h"
#include "thumbDV.h"
#include "dstar.h"
#include "slow_data.h"
#include "circular_buffer.h"

#define SCRAMBLER_TABLE_BITS_LENGTH     720U
#define SCRAMBLER_TABLE_BYTES_LENGTH    90U

static const BOOL SCRAMBLER_TABLE_BITS[] = {
    FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE,
    TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE,
    FALSE, FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE,
    TRUE,  FALSE, TRUE,  TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
    TRUE,  TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,
    TRUE,  FALSE, TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE,
    TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,
    TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE,
    FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,
    TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE,
    FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,
    FALSE, TRUE,  TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,
    TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,
    FALSE, TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,
    TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,
    FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE,
    FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,
    FALSE, FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE,
    TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE,
    TRUE,  TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,
    FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE,
    TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE,
    TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE,
    TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE,
    FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE,
    FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,
    FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, TRUE,
    TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE,
    TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,
    TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,
    TRUE,  TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, TRUE,
    TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE,
    TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE,
    TRUE,  FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE,
    FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE,
    FALSE, TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, TRUE,
    FALSE, TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,
    FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,
    TRUE,  FALSE, TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, TRUE,  TRUE,
    TRUE,  FALSE, FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, TRUE,
    TRUE,  TRUE,  FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,
    FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, FALSE,
    TRUE,  TRUE,  FALSE, FALSE, FALSE, TRUE,  FALSE, TRUE,  TRUE,  TRUE,  FALSE, TRUE,  FALSE, TRUE,  TRUE,  FALSE,
    TRUE,  TRUE,  FALSE, FALSE, FALSE, FALSE, FALSE, TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  FALSE, TRUE,  FALSE,
    TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  FALSE, FALSE, TRUE,  TRUE,  TRUE,  TRUE,  FALSE, TRUE,  TRUE,  FALSE
};



static const unsigned char SCRAMBLER_TABLE_BYTES[] = {
    0x0e, 0xf2, 0xc9, 0x02, 0x26, 0x2e, 0xb6, 0x0c, 0xd4, 0xe7, 0xb4, 0x2a, 0xfa, 0x51, 0xb8, 0xfe,
    0x1d, 0xe5, 0x92, 0x04, 0x4c, 0x5d, 0x6c, 0x19, 0xa9, 0xcf, 0x68, 0x55, 0xf4, 0xa3, 0x71, 0xfc,
    0x3b, 0xcb, 0x24, 0x08, 0x98, 0xba, 0xd8, 0x33, 0x53, 0x9e, 0xd0, 0xab, 0xe9, 0x46, 0xe3, 0xf8,
    0x77, 0x96, 0x48, 0x11, 0x31, 0x75, 0xb0, 0x66, 0xa7, 0x3d, 0xa1, 0x57, 0xd2, 0x8d, 0xc7, 0xf0,
    0xef, 0x2c, 0x90, 0x22, 0x62, 0xeb, 0x60, 0xcd, 0x4e, 0x7b, 0x42, 0xaf, 0xa5, 0x1b, 0x8f, 0xe1,
    0xde, 0x59, 0x20, 0x44, 0xc5, 0xd6, 0xc1, 0x9a, 0x9c, 0xf6
};


static const unsigned int INTERLEAVE_TABLE[] = {
    0, 28, 56, 84, 112, 140, 168, 196, 224, 252, 280, 308, 336, 363, 390, 417, 444,
    471, 498, 525, 552, 579, 606, 633, 1, 29, 57, 85, 113, 141, 169, 197, 225, 253,
    281, 309, 337, 364, 391, 418, 445, 472, 499, 526, 553, 580, 607, 634, 2, 30, 58,
    86, 114, 142, 170, 198, 226, 254, 282, 310, 338, 365, 392, 419, 446, 473, 500,
    527, 554, 581, 608, 635, 3, 31, 59, 87, 115, 143, 171, 199, 227, 255, 283, 311,
    339, 366, 393, 420, 447, 474, 501, 528, 555, 582, 609, 636, 4, 32, 60, 88, 116,
    144, 172, 200, 228, 256, 284, 312, 340, 367, 394, 421, 448, 475, 502, 529, 556,
    583, 610, 637, 5, 33, 61, 89, 117, 145, 173, 201, 229, 257, 285, 313, 341, 368,
    395, 422, 449, 476, 503, 530, 557, 584, 611, 638, 6, 34, 62, 90, 118, 146, 174,
    202, 230, 258, 286, 314, 342, 369, 396, 423, 450, 477, 504, 531, 558, 585, 612,
    639, 7, 35, 63, 91, 119, 147, 175, 203, 231, 259, 287, 315, 343, 370, 397, 424,
    451, 478, 505, 532, 559, 586, 613, 640, 8, 36, 64, 92, 120, 148, 176, 204, 232,
    260, 288, 316, 344, 371, 398, 425, 452, 479, 506, 533, 560, 587, 614, 641, 9,
    37, 65, 93, 121, 149, 177, 205, 233, 261, 289, 317, 345, 372, 399, 426, 453, 480,
    507, 534, 561, 588, 615, 642, 10, 38, 66, 94, 122, 150, 178, 206, 234, 262, 290,
    318, 346, 373, 400, 427, 454, 481, 508, 535, 562, 589, 616, 643, 11, 39, 67, 95,
    123, 151, 179, 207, 235, 263, 291, 319, 347, 374, 401, 428, 455, 482, 509, 536,
    563, 590, 617, 644, 12, 40, 68, 96, 124, 152, 180, 208, 236, 264, 292, 320, 348,
    375, 402, 429, 456, 483, 510, 537, 564, 591, 618, 645, 13, 41, 69, 97, 125, 153,
    181, 209, 237, 265, 293, 321, 349, 376, 403, 430, 457, 484, 511, 538, 565, 592,
    619, 646, 14, 42, 70, 98, 126, 154, 182, 210, 238, 266, 294, 322, 350, 377, 404,
    431, 458, 485, 512, 539, 566, 593, 620, 647, 15, 43, 71, 99, 127, 155, 183, 211,
    239, 267, 295, 323, 351, 378, 405, 432, 459, 486, 513, 540, 567, 594, 621, 648,
    16, 44, 72, 100, 128, 156, 184, 212, 240, 268, 296, 324, 352, 379, 406, 433, 460,
    487, 514, 541, 568, 595, 622, 649, 17, 45, 73, 101, 129, 157, 185, 213, 241, 269,
    297, 325, 353, 380, 407, 434, 461, 488, 515, 542, 569, 596, 623, 650, 18, 46, 74,
    102, 130, 158, 186, 214, 242, 270, 298, 326, 354, 381, 408, 435, 462, 489, 516,
    543, 570, 597, 624, 651, 19, 47, 75, 103, 131, 159, 187, 215, 243, 271, 299, 327,
    355, 382, 409, 436, 463, 490, 517, 544, 571, 598, 625, 652, 20, 48, 76, 104, 132,
    160, 188, 216, 244, 272, 300, 328, 356, 383, 410, 437, 464, 491, 518, 545, 572,
    599, 626, 653, 21, 49, 77, 105, 133, 161, 189, 217, 245, 273, 301, 329, 357, 384,
    411, 438, 465, 492, 519, 546, 573, 600, 627, 654, 22, 50, 78, 106, 134, 162, 190,
    218, 246, 274, 302, 330, 358, 385, 412, 439, 466, 493, 520, 547, 574, 601, 628,
    655, 23, 51, 79, 107, 135, 163, 191, 219, 247, 275, 303, 331, 359, 386, 413, 440,
    467, 494, 521, 548, 575, 602, 629, 656, 24, 52, 80, 108, 136, 164, 192, 220, 248,
    276, 304, 332, 360, 387, 414, 441, 468, 495, 522, 549, 576, 603, 630, 657, 25, 53,
    81, 109, 137, 165, 193, 221, 249, 277, 305, 333, 361, 388, 415, 442, 469, 496, 523,
    550, 577, 604, 631, 658, 26, 54, 82, 110, 138, 166, 194, 222, 250, 278, 306, 334,
    362, 389, 416, 443, 470, 497, 524, 551, 578, 605, 632, 659, 27, 55, 83, 111, 139,
    167, 195, 223, 251, 279, 307, 335
};

static const unsigned short ccittTab[] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
};

void icom_byteToBits( unsigned char byte, BOOL * bits ) {
    unsigned char mask = 0x01;
    uint32 i = 0;

    for ( i = 0 ; i < 8 ; i++, mask <<= 1 ) {
        bits[i] = ( byte & mask ) ? TRUE : FALSE;
    }
}

void dstar_scramble( BOOL * in, BOOL * out, uint32 length, uint32 * scramble_count ) {
    if ( out == NULL || in == NULL || scramble_count == NULL ) {
        output( "Null inOut pointer\n" );
        return;
    }

    uint32 i = 0;

    for ( i = 0 ; i < length ; i++ ) {
        out[i] = in[i] ^ SCRAMBLER_TABLE_BITS[( *scramble_count )++];

        if ( *scramble_count >= SCRAMBLER_TABLE_BITS_LENGTH )
            *scramble_count = 0U;
    }
}


void dstar_interleave( const BOOL * in, BOOL * out, unsigned int length ) {
    if ( in == NULL || out == NULL ) {
        output( ANSI_RED "Null in or out in interleave\n" ANSI_WHITE );
        return;
    }

    if ( length != FEC_SECTION_LENGTH_BITS ) {
        output( ANSI_RED "Wrong leangth in interleave\n" ANSI_WHITE );
    }

    memset( out, 0, FEC_SECTION_LENGTH_BITS * sizeof( BOOL ) );
    uint32 i = 0;

    for ( i = 0 ; i < FEC_SECTION_LENGTH_BITS ; i++ ) {
        if ( in[i] ) {
            unsigned int newi = INTERLEAVE_TABLE[i];

            if ( newi >= FEC_SECTION_LENGTH_BITS ) {
                output( ANSI_RED "Out of range index interleave\n" ANSI_WHITE );
            }

            out[newi] = TRUE;
        }

    }
}

void dstar_deinterleave( const BOOL * in, BOOL * out, unsigned int length ) {

    memset( out, 0, FEC_SECTION_LENGTH_BITS * sizeof( BOOL ) );

    uint32 k = 0;
    uint32 i = 0;

    for ( i = 0 ; i < length ; i++ ) {
        if ( k >=  FEC_SECTION_LENGTH_BITS ) {
            output( ANSI_RED "k greater than FEC_SECTION_LENGTH_BITS - deinterleave\n" ANSI_WHITE );
        }

        if ( in[i] )
            out[k] = TRUE;
        else
            out[k] = FALSE;

        k += 24U;

        if ( k >= 672U )
            k -= 671U;
        else if ( k >= 660U )
            k -= 647U;
    }
}


/****** FEC DECOE ************/

enum FEC_STATE {
    S0,
    S1,
    S2,
    S3
};


void dstar_FECacs( DSTAR_FEC fec, unsigned int n, int * metric ) {
    //output("ACS\n");
    int tempMetric[4];

    // Pres. state = S0, Prev. state = S0 & S2
    int m1 = metric[0] + fec->metric[0];
    int m2 = metric[4] + fec->metric[2];

    if ( m1 < m2 ) {
        fec->mem0[n] = FALSE;
        tempMetric[0]    = m1;
    } else {
        fec->mem0[n] = TRUE;
        tempMetric[0]    = m2;
    }

    // Pres. state = S1, Prev. state = S0 & S2
    m1 = metric[1] + fec->metric[0];
    m2 = metric[5] + fec->metric[2];

    if ( m1 < m2 ) {
        fec->mem1[n] = FALSE;
        tempMetric[1]    = m1;
    } else {
        fec->mem1[n] = TRUE;
        tempMetric[1]    = m2;
    }

    // Pres. state = S2, Prev. state = S2 & S3
    m1 = metric[2] + fec->metric[1];
    m2 = metric[6] + fec->metric[3];

    if ( m1 < m2 ) {
        fec->mem2[n] = FALSE;
        tempMetric[2]    = m1;
    } else {
        fec->mem2[n] = TRUE;
        tempMetric[2]    = m2;
    }

    // Pres. state = S3, Prev. state = S1 & S3
    m1 = metric[3] + fec->metric[1];
    m2 = metric[7] + fec->metric[3];

    if ( m1 < m2 ) {
        fec->mem3[n] = FALSE;
        tempMetric[3]    = m1;
    } else {
        fec->mem3[n] = TRUE;
        tempMetric[3]    = m2;
    }

    uint32 i = 0;

    for ( i = 0 ; i < 4 ; i++ )
        fec->metric[i] = tempMetric[i];
}


void dstar_FECtraceBack( DSTAR_FEC fec, BOOL * out, unsigned int * length ) {
    //output("traceBack\n");
    // Start from the S0, t=31
    enum FEC_STATE state = S0;

    *length = 0U;
    int i = 0;

    for ( i = 329 ; i >= 0 ; i--, ( *length )++ ) {
        switch ( state ) {
        case S0: // if state = S0

            //output("i = %d\n");
            if ( fec->mem0[i] )
                state = S2;             // lower path
            else
                state = S0;             // upper path

            out[i] = FALSE;
            break;

        case S1: // if state = S1
            if ( fec->mem1[i] )
                state = S2;             // lower path
            else
                state = S0;             // upper path

            out[i] = TRUE;
            break;

        case S2: // if state = S2
            if ( fec->mem2[i] )
                state = S3;             // lower path
            else
                state = S1;             // upper path

            out[i] = FALSE;
            break;

        case S3: // if state = S3
            if ( fec->mem3[i] )
                state = S3;             // lower path
            else
                state = S1;             // upper path

            out[i] = TRUE;
            break;
        }
    }
}


void dstar_FECviterbiDecode( DSTAR_FEC fec, unsigned int n, int * data ) {
    //output("ViterbiDecode\n");
    int metric[8];

    metric[0] = ( data[1] ^ 0 ) + ( data[0] ^ 0 );
    metric[1] = ( data[1] ^ 1 ) + ( data[0] ^ 1 );
    metric[2] = ( data[1] ^ 1 ) + ( data[0] ^ 0 );
    metric[3] = ( data[1] ^ 0 ) + ( data[0] ^ 1 );
    metric[4] = ( data[1] ^ 1 ) + ( data[0] ^ 1 );
    metric[5] = ( data[1] ^ 0 ) + ( data[0] ^ 0 );
    metric[6] = ( data[1] ^ 0 ) + ( data[0] ^ 1 );
    metric[7] = ( data[1] ^ 1 ) + ( data[0] ^ 0 );

    dstar_FECacs( fec, n, metric );
}


BOOL dstar_FECdecode( DSTAR_FEC fec, const BOOL * in, BOOL * out, unsigned int inLen, unsigned int * outLen ) {
    if ( in == NULL || out == NULL ) {
        output( ANSI_RED "NULL in or out in FECDecode\n" ANSI_WHITE );
        return FALSE;
    }

    uint32 i = 0;

    for ( i = 0U; i < 4U; i++ )
        fec->metric[i] = 0;

    unsigned int n = 0U;

    for ( i = 0U; i < FEC_SECTION_LENGTH_BITS; i += 2U, n++ ) {
        int data[2];

        if ( in[i + 0U] )
            data[1] = 1;
        else
            data[1] = 0;

        if ( in[i + 1U] )
            data[0] = 1;
        else
            data[0] = 0;

        dstar_FECviterbiDecode( fec, n, data );
    }

    dstar_FECtraceBack( fec, out, outLen );

    // Swap endian-ness
    for ( i = 0U; i < RADIO_HEADER_LENGTH_BITS; i += 8U ) {
        BOOL temp;
        temp = out[i + 0U];
        out[i + 0U] = out[i + 7U];
        out[i + 7U] = temp;
        temp = out[i + 1U];
        out[i + 1U] = out[i + 6U];
        out[i + 6U] = temp;
        temp = out[i + 2U];
        out[i + 2U] = out[i + 5U];
        out[i + 5U] = temp;
        temp = out[i + 3U];
        out[i + 3U] = out[i + 4U];
        out[i + 4U] = temp;
    }

    return TRUE;
}


void dstar_FECencode( const BOOL * in, BOOL * out, unsigned int inLen, unsigned int * outLen ) {

    *outLen = 0U;
    int d1 = 0;
    int d2 = 0;
    uint32 i = 0;
    int32 j = 0;

    for ( i = 0U; i < 42U; i++ ) {
        for ( j = 7; j >= 0; j-- ) {
            int d = in[i * 8U + j] ? 1 : 0;

            int g0 = ( d + d2 ) % 2;
            int g1 = ( d + d1 + d2 ) % 2;

            d2 = d1;
            d1 = d;

            out[( *outLen )++] = g1 == 1;
            out[( *outLen )++] = g0 == 1;
        }
    }
}


void dstar_createTestHeader( DSTAR_HEADER header ) {
    strcpy( header->departure_rptr, "DIRECT  " );
    strcpy( header->destination_rptr, "DIRECT  " );
    strcpy( header->companion_call, "CQCQCQ  " );
    strcpy( header->own_call1, "CALLSIGN" );
    strcpy( header->own_call2, "    " );
}

DSTAR_MACHINE dstar_createMachine( void ) {
    DSTAR_MACHINE machine = safe_malloc( sizeof( dstar_machine ) );
    memset( machine, 0, sizeof( dstar_machine ) );

    machine->rx_state = BIT_FRAME_SYNC;
    machine->tx_state = BIT_FRAME_SYNC;

    BOOL syn_bits[15 + 4] = {0};
    uint32 i = 0;

    syn_bits[0] = TRUE;
    syn_bits[1] = FALSE;
    syn_bits[2] = TRUE;
    syn_bits[3] = FALSE;


    BOOL frame_bits[] = {TRUE, TRUE,  TRUE, FALSE, TRUE,  TRUE,  FALSE, FALSE,
                         TRUE, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE
                        };

    for ( i = 4 ; i < 15 + 4 ; i++ ) {
        syn_bits[i] = frame_bits[i - 4];
    }

    machine->syn_pm = bitPM_create( syn_bits, 15 + 4 );
    machine->data_sync_pm = bitPM_create( DATA_SYNC_BITS, 24 );
    machine->end_pm = bitPM_create( END_PATTERN_BITS, END_PATTERN_LENGTH_BITS );


    dstar_createTestHeader( &( machine->outgoing_header ) );

    machine->slow_decoder = safe_malloc(sizeof(slow_data_decoder));
    machine->slow_encoder = safe_malloc(sizeof(slow_data_encoder));

    machine->slice = 0;

    return machine;
}

void dstar_destroyMachine( DSTAR_MACHINE machine ) {
    bitPM_destroy( machine->syn_pm );
    bitPM_destroy( machine->data_sync_pm );
    bitPM_destroy( machine->end_pm );

    safe_free(machine->slow_decoder);
    safe_free(machine->slow_encoder);

    safe_free( machine );
}

void dstar_dumpHeader( DSTAR_HEADER header ) {
    output( "HEADER:\n" );
    output( "Flag1: 0x%08X\n", header->flag1 );
    output( "Flag2: 0x%08X\n", header->flag2 );
    output( "Flag3: 0x%08X\n", header->flag3 );
    output( "Destination RPTR: '%s'\n", header->destination_rptr );
    output( "Departure RPTR: '%s'\n", header->departure_rptr );
    output( "Companion Call: '%s'\n", header->companion_call );
    output( "Own Call 1: '%s'\n", header->own_call1 );
    output( "Own Call 2: '%s'\n", header->own_call2 );
}

void dstar_headerToBytes( DSTAR_HEADER header, unsigned char * bytes ) {
    memset( bytes, 0, 39 * sizeof( unsigned char ) );

    bytes[0] = header->flag1;
    bytes[1] = header->flag2;
    bytes[2] = header->flag3;

    memcpy( &bytes[3], header->destination_rptr, 8 );
    memcpy( &bytes[3 + 8], header->departure_rptr, 8 );
    memcpy( &bytes[3 + 8 + 8], header->companion_call, 8 );
    memcpy( &bytes[3 + 8 + 8 + 8], header->own_call1, 8 );
    memcpy( &bytes[3 + 8 + 8 + 8 + 8], header->own_call2, 4 );
}

void dstar_processHeader( unsigned char * bytes, DSTAR_HEADER header ) {
    /* Takes in an array of bytes and parses out each header field */
    memset( header, 0, sizeof( dstar_header ) );

    header->flag1 = bytes[0];
    header->flag2 = bytes[1];
    header->flag3 = bytes[2];

    memcpy( header->destination_rptr, &bytes[3], 8 );
    memcpy( header->departure_rptr, &bytes[3 + 8], 8 );
    memcpy( header->companion_call, &bytes[3 + 8 + 8], 8 );
    memcpy( header->own_call1, &bytes[3 + 8 + 8 + 8], 8 );
    memcpy( header->own_call2, &bytes[3 + 8 + 8 + 8 + 8], 4 );

    dstar_dumpHeader( header );
}

static unsigned char icom_bitsToByte( const BOOL * bits ) {
    uint32 l = 0;

    unsigned char val = 0x00;

    for ( l = 0 ; l < 8 ; l++ ) {
        val >>= 1;

        if ( bits[l] ) {
            val |= 0x80;
        }
    }

    return val;
}

void dstar_updateStatus( DSTAR_MACHINE machine, uint32 slice,  enum STATUS_TYPE type ) {
    if ( machine == NULL ) {
        output( ANSI_RED "NULL dStar machine %d\n" ANSI_WHITE, __LINE__ );
        return;
    }

    char status[200] = {0};
    char header_string[256] = {0};
	char message_string[21];


    /* Make copy to replace spaces with special char */
    dstar_header h;

    switch ( type ) {

    case STATUS_RX:
        memcpy( &h, &( machine->incoming_header ), sizeof( dstar_header ) );

        charReplace( ( char * )h.destination_rptr, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.departure_rptr, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.companion_call, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.own_call1, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.own_call2, ' ', ( char ) 0x7F );

        sprintf( header_string, "destination_rptr_rx=%s departure_rptr_rx=%s companion_call_rx=%s own_call1_rx=%s own_call2_rx=%s",
                 h.destination_rptr, h.departure_rptr, h.companion_call, h.own_call1, h.own_call2 );

        sprintf( status, "waveform status slice=%d %s", slice, header_string );

        tc_sendSmartSDRcommand( status, FALSE, NULL );
        break;

    case STATUS_TX:

        memcpy( &h, &( machine->outgoing_header ), sizeof( dstar_header ) );

        charReplace( ( char * )h.destination_rptr, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.departure_rptr, ' ', ( char ) 0x7F );
		charReplace((char *) h.companion_call, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.own_call1, ' ', ( char ) 0x7F );
        charReplace( ( char * )h.own_call2, ' ', ( char ) 0x7F );

        sprintf( header_string, "destination_rptr_tx=%s departure_rptr_tx=%s companion_call_tx=%s own_call1_tx=%s own_call2_tx=%s",
                 h.destination_rptr, h.departure_rptr, h.companion_call, h.own_call1, h.own_call2 );

        if (machine->slow_encoder != NULL && machine->slow_encoder->message[0] != 0)
        {
            memcpy( message_string, machine->slow_encoder->message, sizeof( message_string ) );
            message_string[sizeof( message_string ) - 1] = 0;
            charReplace( message_string, ' ', ( char ) 0x7F );
		sprintf( header_string + strlen(header_string), " message_tx=%s", message_string);
        }

        sprintf( status, "waveform status slice=%d %s", slice, header_string );

        tc_sendSmartSDRcommand( status, FALSE, NULL );
        break;
    case STATUS_SLOW_DATA_MESSAGE:
        memcpy( message_string, machine->slow_decoder->message_string, sizeof( message_string ) );
        message_string[sizeof( message_string ) - 1] = 0;
        charReplace( message_string, ' ', ( char ) 0x7F );
        sprintf( status, "waveform status slice=%d message=%s", slice, message_string);
        tc_sendSmartSDRcommand( status, FALSE, NULL );
        break;
    case STATUS_END_RX:
    {
        char msg[64];
        sprintf( msg, "waveform status slice=%d RX=END", machine->slice);
        tc_sendSmartSDRcommand( msg, FALSE, NULL );
    }
        break;

    }
}

void dstar_txStateMachine( DSTAR_MACHINE machine, GMSK_MOD gmsk_mod, Circular_Float_Buffer tx_cb, unsigned char * mod_audio)
{
    uint32 i = 0;
    uint32 j = 0;
    float buf[DSTAR_RADIO_BIT_LENGTH];
    dstar_pfcs pfcs;
#ifdef DUMP_GMSK_MOD
    static FILE * dump_file = NULL;

    if ( dump_file == NULL ) {
        dump_file = fopen("/tmp/gmsk_encoding.dat", "w");
    }
#endif

    switch ( machine->tx_state ) {
    case BIT_FRAME_SYNC:
        /* Create Sync */
        for ( i = 0 ; i < 64 * 5; i += 2 ) {
            gmsk_encode( gmsk_mod, TRUE, buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }

            gmsk_encode( gmsk_mod, FALSE, buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }
        }

        for ( i = 0 ; i < FRAME_SYNC_LENGTH_BITS ; i++ ) {
            gmsk_encode( gmsk_mod, FRAME_SYNC_BITS[i], buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }
        }
        break;
    case HEADER_PROCESSING:

        pfcs.crc16 = 0xFFFF;

        unsigned char header_bytes[RADIO_HEADER_LENGTH_BITS] = {0};
        dstar_headerToBytes( &( machine->outgoing_header ), header_bytes );
        dstar_pfcsUpdateBuffer( &pfcs, header_bytes, 312 / 8 );
        dstar_pfcsResult( &pfcs, header_bytes + 312 / 8 );

        output( "Main: PFCS Bytes: 0x%08X 0x%08X\n", *( header_bytes + 312 / 8 ), *( header_bytes + 320 / 8 ) );

        BOOL bits[FEC_SECTION_LENGTH_BITS] = {0};

        gmsk_bytesToBits( header_bytes, bits, 328 );
        BOOL encoded[RADIO_HEADER_LENGTH_BITS * 2] = {0};
        BOOL interleaved[RADIO_HEADER_LENGTH_BITS * 2] = {0};
        BOOL scrambled[RADIO_HEADER_LENGTH_BITS * 2] = {0};
        uint32 outLen = 0;
        dstar_FECencode( bits, encoded, RADIO_HEADER_LENGTH_BITS, &outLen );
        //output("Encode outLen = %d\n", outLen);

        outLen = FEC_SECTION_LENGTH_BITS;
        dstar_interleave( encoded, interleaved, outLen );

        uint32 count = 0;
        dstar_scramble( interleaved, scrambled, outLen, &count );
        //output( "Count = %d\n", count );

        for ( i = 0 ; i < count ; i++ ) {
            gmsk_encode( gmsk_mod, scrambled[i], buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }
        }
        break;
    case VOICE_FRAME:
    {
        BOOL bits[8] = {0} ;
        uint32 k = 0;

        for ( i = 0 ; i < VOICE_FRAME_LENGTH_BYTES ; i++ ) {
            icom_byteToBits( mod_audio[i], bits );

            for ( j = 0 ; j < 8 ; j++ ) {
                gmsk_encode( gmsk_mod, bits[j], buf, DSTAR_RADIO_BIT_LENGTH );

                for ( k = 0 ; k < DSTAR_RADIO_BIT_LENGTH ; k++ ) {
                    cbWriteFloat( tx_cb, buf[k] );
#ifdef DUMP_GMSK_MOD
                    fprintf(dump_file, "%6.6f\n", buf[k]);
#endif
                }
            }
        }
        break;
    }
    case DATA_FRAME:
    {
        unsigned char encode_bytes[SLOW_DATA_PACKET_LEN_BYTES] = {0};
        BOOL encode_bits[DATA_FRAME_LENGTH_BITS] = {0};
        BOOL encode_bits_scrambled[DATA_FRAME_LENGTH_BITS] = {0};
        unsigned char encode_bytes_scrambled[SLOW_DATA_PACKET_LEN_BYTES] = {0};
        uint32 count = 0;
        float data_buf[DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH ] = {0};

        slow_data_getEncodeBytes(machine, encode_bytes, SLOW_DATA_PACKET_LEN_BYTES);

        for ( i = 0, j = 0 ; i < SLOW_DATA_PACKET_LEN_BYTES ; i++, j += 8 ) {
            icom_byteToBits( encode_bytes[i], encode_bits + j);
        }

        dstar_scramble(encode_bits, encode_bits_scrambled, DATA_FRAME_LENGTH_BITS, &count);


        gmsk_bitsToBytes(encode_bits_scrambled, encode_bytes_scrambled, DATA_FRAME_LENGTH_BITS);

        gmsk_encodeBuffer(gmsk_mod, encode_bytes_scrambled, DATA_FRAME_LENGTH_BITS, data_buf, DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH);

        for ( i = 0 ; i < DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH ; i++ ) {
            cbWriteFloat(tx_cb, data_buf[i]);
#ifdef DUMP_GMSK_MOD
            fprintf(dump_file, "%6.6f\n", data_buf[i]);
#endif
        }

        break;
    }
    case DATA_SYNC_FRAME:
    {
        /* Sync Bits */
        unsigned char sync_bytes[3] = {0};
        float sync_buf[DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH] = {0};
        memcpy( sync_bytes, DATA_SYNC_BYTES, 3 );
        gmsk_encodeBuffer( gmsk_mod, sync_bytes, DATA_FRAME_LENGTH_BITS, sync_buf, DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH );

        for ( i = 0 ; i < DATA_FRAME_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH ; i++ ) {
            cbWriteFloat( tx_cb, sync_buf[i] );
#ifdef DUMP_GMSK_MOD
            fprintf(dump_file, "%6.6f\n", sync_buf[i]);
#endif
        }

        break;
    }
    case END_PATTERN:
    {
        float end_buf[END_PATTERN_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH] = {0.0};
        unsigned char end_bytes[END_PATTERN_LENGTH_BYTES] = {0};
        memcpy( end_bytes, END_PATTERN_BYTES, END_PATTERN_LENGTH_BYTES * sizeof( unsigned char ) );
        gmsk_encodeBuffer( gmsk_mod, end_bytes, END_PATTERN_LENGTH_BITS, end_buf, END_PATTERN_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH );

        for ( i = 0 ; i < END_PATTERN_LENGTH_BITS * DSTAR_RADIO_BIT_LENGTH ; i++ ) {
            cbWriteFloat( tx_cb, end_buf[i] );
#ifdef DUMP_GMSK_MOD
            fprintf(dump_file, "%6.6f\n", end_buf[i]);
#endif
        }

        for ( i = 0 ; i <  22 ; i += 2 ) {
            gmsk_encode( gmsk_mod, TRUE, buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }

            gmsk_encode( gmsk_mod, FALSE, buf, DSTAR_RADIO_BIT_LENGTH );

            for ( j = 0 ; j < DSTAR_RADIO_BIT_LENGTH ; j++ ) {
                cbWriteFloat( tx_cb, buf[j] );
#ifdef DUMP_GMSK_MOD
                fprintf(dump_file, "%6.6f\n", buf[j]);
#endif
            }
        }

#ifdef DUMP_GMSK_MOD
      fclose(dump_file);
      dump_file = NULL;
#endif

        slow_data_resetEncoder(machine);

        break;
    }
    }
}

BOOL dstar_rxStateMachine( DSTAR_MACHINE machine, BOOL in_bit, unsigned char * ambe_out, uint32 ambe_buf_len ) {
    BOOL have_audio_packet = FALSE;
    BOOL found_syn_bits = FALSE;
    BOOL found_end_bits = FALSE;
    BOOL * header = machine->header;
    BOOL * voice_bits = machine->voice_bits;
    BOOL * data_bits = machine->data_bits;

    unsigned char bytes[FEC_SECTION_LENGTH_BITS / 8 + 1];

    switch ( machine->rx_state ) {
    case BIT_FRAME_SYNC:
        found_syn_bits = bitPM_addBit( machine->syn_pm, in_bit );
        BOOL found_data_sync = bitPM_addBit( machine->data_sync_pm, in_bit );

        machine->slow_decoder->decode_state = FIRST_FRAME;
        machine->slow_decoder->header_array_index = 0;

        if ( found_syn_bits ) {
            output( "FOUND SYN BITS\n" );
            bitPM_reset( machine->syn_pm );
            bitPM_reset( machine->data_sync_pm );
            machine->rx_state = HEADER_PROCESSING;
            machine->bit_count = 0;
        } else if ( found_data_sync ) {
            output( "FOUND DATA SYNC BITS instead of header\n" );
            bitPM_reset( machine->syn_pm );
            bitPM_reset( machine->data_sync_pm );
            machine->rx_state = VOICE_FRAME;
            machine->bit_count = 0;
            machine->frame_count++;
        }
        break;

    case HEADER_PROCESSING:
        header[machine->bit_count++] = in_bit;

        if ( machine->bit_count == FEC_SECTION_LENGTH_BITS ) {
            // output("Found 660 bits - descrambling\n");
            /* Found 660 bits of header */

//                gmsk_bitsToBytes(header, bytes, FEC_SECTION_LENGTH_BITS);
//                thumbDV_dump("RAW:", bytes, FEC_SECTION_LENGTH_BITS/8);

            uint32 scramble_count = 0;
            BOOL descrambled[FEC_SECTION_LENGTH_BITS] = {0};
            dstar_scramble( header, descrambled, FEC_SECTION_LENGTH_BITS, &scramble_count );
//                gmsk_bitsToBytes(descrambled, bytes, FEC_SECTION_LENGTH_BITS);
//                thumbDV_dump("DESCRAMBLE:", bytes, FEC_SECTION_LENGTH_BITS/8);

            BOOL out[FEC_SECTION_LENGTH_BITS] = {0};
            dstar_deinterleave( descrambled, out, FEC_SECTION_LENGTH_BITS );
//                gmsk_bitsToBytes(out, bytes, FEC_SECTION_LENGTH_BITS);
//                thumbDV_dump("DEINTERLEAVE:", bytes, FEC_SECTION_LENGTH_BITS/8);
            dstar_fec fec;
            memset( &fec, 0, sizeof( dstar_fec ) );
            unsigned int outLen = FEC_SECTION_LENGTH_BITS;
            BOOL decoded[RADIO_HEADER_LENGTH_BITS] = {0};
            dstar_FECdecode( &fec, out, decoded, FEC_SECTION_LENGTH_BITS, &outLen );
//                output("outLen = %d\n" ,outLen);
            gmsk_bitsToBytes( decoded, bytes, outLen );
//                thumbDV_dump("FEC: ", bytes, outLen/8);
            uint32 i = 0;
            dstar_pfcs pfcs;
            pfcs.crc16 = 0xFFFF;

            for ( i = 0 ; i < 312 ; i += 8 ) {
                dstar_pfcsUpdate( &pfcs, decoded + i );
            }

            BOOL pfcs_match = FALSE;
            pfcs_match = dstar_pfcsCheck( &pfcs, decoded + 312 );

            if ( pfcs_match ) {
                output( ANSI_GREEN "P_FCS Matches!\n" ANSI_WHITE );

                dstar_processHeader( bytes, &machine->incoming_header );

                dstar_updateStatus( machine, machine->slice, STATUS_RX );

                machine->rx_state = VOICE_FRAME;
                machine->bit_count = 0;
                machine->frame_count = 0;


            } else {
                output( ANSI_RED "P_FCS Does Not Match!\n" ANSI_WHITE );

                machine->rx_state = BIT_FRAME_SYNC;
                machine->bit_count = 0;
            }

            /* STATE CHANGE */

        }

        break;

    case VOICE_FRAME:
        voice_bits[machine->bit_count++] = in_bit;

        found_end_bits = bitPM_addBit( machine->end_pm, in_bit );

        if ( found_end_bits ) {
            machine->rx_state = END_PATTERN;
            machine->bit_count = 0;
        } else if ( machine->bit_count == VOICE_FRAME_LENGTH_BITS ) {
            memset( bytes, 0, VOICE_FRAME_LENGTH_BYTES );
            uint32 n = 0;
            uint32 i = 0 ;

            for ( i = 0, n = 0 ; i < VOICE_FRAME_LENGTH_BYTES ; i++, n += 8 ) {
                bytes[i] = icom_bitsToByte( voice_bits + n );
            }

            //thumbDV_dump("ICOM Order: " , bytes, VOICE_FRAME_LENGTH_BITS / 8);
            memcpy( ambe_out, bytes, VOICE_FRAME_LENGTH_BITS / 8 );
            have_audio_packet = TRUE;

            /* STATE CHANGE */
            if ( machine->frame_count % 21 == 0 ) {
                /* Expecting a SYNC FRAME */
                machine->rx_state = DATA_SYNC_FRAME;
            } else {
                machine->rx_state = DATA_FRAME;
            }

            machine->bit_count = 0;
        }

        break;

    case DATA_FRAME:
        data_bits[machine->bit_count++] = in_bit;

        found_end_bits = bitPM_addBit( machine->end_pm, in_bit );

        if ( found_end_bits ) {
            machine->rx_state = END_PATTERN;
            machine->bit_count = 0;
        } else if ( machine->bit_count == DATA_FRAME_LENGTH_BITS ) {

            BOOL out[DATA_FRAME_LENGTH_BITS] = {0};
            uint32 scramble_count = 0;
            dstar_scramble( data_bits, out,  DATA_FRAME_LENGTH_BITS, &scramble_count );

            uint32 i = 0 ;
            uint32 n = 0;

            for ( i = 0, n = 0  ; i < DATA_FRAME_LENGTH_BYTES ; i++, n += 8 ) {
                bytes[i]  = icom_bitsToByte( out + n );
            }

            slow_data_addDecodeData(machine, bytes, DATA_FRAME_LENGTH_BYTES);

            machine->frame_count++;

            /* STATE CHANGE */
            machine->rx_state = VOICE_FRAME;
            machine->bit_count = 0;
        }

        break;

    case DATA_SYNC_FRAME: {
        BOOL found_sync = FALSE;
        found_sync = bitPM_addBit( machine->data_sync_pm, in_bit );
        machine->bit_count++;

        found_end_bits = bitPM_addBit( machine->end_pm, in_bit );

        if ( found_sync ) {
            output( "Found Sync\n" );

            machine->frame_count++;

            bitPM_reset( machine->data_sync_pm );
            /* STATE CHANGE */
            machine->rx_state = VOICE_FRAME;
            machine->bit_count = 0;
        } else if ( found_end_bits ) {
            machine->rx_state = END_PATTERN;
            machine->bit_count = 0;
        } else if ( machine->bit_count > ( ( DATA_FRAME_LENGTH_BITS + VOICE_FRAME_LENGTH_BITS ) * 42 ) ) {
            /* Function as a timeout if we don't find the sync bits */
            output( "Could not find SYNC\n" );

            bitPM_reset( machine->data_sync_pm );

            dstar_updateStatus(machine, machine->slice, STATUS_END_RX);
            /* STATE CHANGE */
            machine->rx_state = BIT_FRAME_SYNC;
            machine->bit_count = 0;
        }

        slow_data_resetDecoder(machine);

        break;
    }

    case END_PATTERN:

        output( "Found end pattern bits -- sending status update\n" );

        dstar_updateStatus(machine, machine->slice, STATUS_END_RX);

        bitPM_reset( machine->end_pm );
        bitPM_reset( machine->syn_pm );

        /* STATE CHANGE */
        machine->rx_state = BIT_FRAME_SYNC;
        machine->bit_count = 0;

        break;

    default:
        output( ANSI_YELLOW "Unhandled rx_state - dstar_stateMachine. State = 0x%08X" ANSI_WHITE, machine->rx_state );
        break;
    }

    return have_audio_packet;
}

void dstar_pfcsUpdateBuffer( DSTAR_PFCS pfcs, unsigned char * bytes, uint32 length ) {
    uint32 i = 0;

    for ( i = 0 ; i < length ; i++ ) {
        pfcs->crc16 = ( uint16 )( pfcs->crc8[1] ) ^ ccittTab[pfcs->crc8[0] ^ bytes[i]];
    }
}

void dstar_pfcsUpdate( DSTAR_PFCS pfcs, BOOL * bits ) {
    unsigned char byte;
    gmsk_bitsToByte( bits, &byte );

    pfcs->crc16 = ( uint16 )pfcs->crc8[1] ^ ccittTab[ pfcs->crc8[0] ^ byte ];
}

void dstar_pfcsResult( DSTAR_PFCS pfcs, unsigned char * chksum ) {
    pfcs->crc16 = ~ pfcs->crc16;

    chksum[0] = pfcs->crc8[0];
    chksum[1] = pfcs->crc8[1];

}

void dstar_pfcsResultBits( DSTAR_PFCS pfcs, BOOL * bits ) {
    pfcs->crc16 = ~pfcs->crc16;

    unsigned char mask = 0x80;
    uint32 i = 0;

    for ( i = 0 ; i < 8 ; i++, mask >>= 1 ) {
        bits[i + 0] = ( pfcs->crc8[0] & mask ) ? TRUE : FALSE;
    }

    mask = 0x80;

    for ( i = 0 ; i < 8 ; i++ , mask >>= 1 ) {
        bits[i + 8] = ( pfcs->crc8[1] & mask ) ? TRUE : FALSE;
    }
}

BOOL dstar_pfcsCheck( DSTAR_PFCS pfcs, BOOL * bits ) {
    uint32 i = 0;
    BOOL sum[16];
    dstar_pfcsResultBits( pfcs, sum );

    for ( i = 0 ; i < 16 ; i++ ) {
        if ( sum[i] != bits[i] ) {
            return FALSE;
        }
    }

    return TRUE;
}



void dstar_FECTest( void ) {
    unsigned char bytes[660 / 8] = {0};
    BOOL test[330] = {0};
    BOOL encoded[330 * 2] = {0};
    BOOL interleaved[330 * 2] = {0};
    BOOL deinterleaved[330 * 2] = {0};
    BOOL scrambled[330 * 2] = {0};
    BOOL descrambled[330 * 2] = {0};
    BOOL decoded[330] = {0};
    uint32 i = 0;

    for ( i = 0 ; i < 327 - 1 ; i += 2 ) {
        test[i] = TRUE;//(rand() & 0x01) ? TRUE:FALSE;
        test[i + 1] = FALSE; //( rand() & 0x01 ) ? TRUE:FALSE ;
    }

    gmsk_bitsToBytes( test, bytes, 330 );
    thumbDV_dump( "TEST FEC IN:", bytes, 330 / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );

    uint32 outLen = 0;
    dstar_FECencode( test, encoded, 300, &outLen );
    output( "Encode outLen = %d\n", outLen );
    outLen = 660;
    gmsk_bitsToBytes( encoded, bytes, outLen );
    thumbDV_dump( "TEST FEC ENCODE", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );

    dstar_interleave( encoded, interleaved, outLen );
    gmsk_bitsToBytes( interleaved, bytes, outLen );
    thumbDV_dump( "TEST INTERLEAVE", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );

    uint32 count = 0;
    dstar_scramble( interleaved, scrambled, outLen, &count );
    gmsk_bitsToBytes( scrambled, bytes, outLen );
    thumbDV_dump( "TEST SCRAMBLE", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );

    count = 0;
    dstar_scramble( scrambled, descrambled, outLen, &count );
    gmsk_bitsToBytes( descrambled, bytes, outLen );
    thumbDV_dump( "TEST DE-SCRAMBLE", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );

    dstar_deinterleave( descrambled, deinterleaved, outLen );
    gmsk_bitsToBytes( deinterleaved, bytes, outLen );
    thumbDV_dump( "TEST DE-INTELEAVE", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );


    dstar_fec fec;
    memset( &fec, 0, sizeof( dstar_fec ) );
    output( "outLen = %d\n", outLen );
    dstar_FECdecode( &fec, deinterleaved, decoded, outLen, &outLen );
    output( "Decode outLen = %d\n", outLen );
    gmsk_bitsToBytes( decoded, bytes, outLen );
    thumbDV_dump( "TEST FEC Decode", bytes, outLen / 8 );
    memset( bytes, 0, 660 / 8 * sizeof( unsigned char ) );
}
