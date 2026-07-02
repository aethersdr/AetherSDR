///*!   \file gmsk_modem.h
// *    \date 02-JUN-2015
// *    \author Ed Gonzalez KG5FBT
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

#ifndef THUMBDV_GMSK_MODEM_H_
#define THUMBDV_GMSK_MODEM_H_

#include "common.h"

enum DEMOD_STATE {
    DEMOD_FALSE,
    DEMOD_TRUE,
    DEMOD_UNKNOWN
};

#define SC_EQ_MAX_DEPTH     3
#define SC_EQ_FILTER_LEN    3

typedef struct _iir_filter
{
    float in_line[SC_EQ_FILTER_LEN];
    float out_line[SC_EQ_FILTER_LEN];
    float a[SC_EQ_FILTER_LEN];
    float b[SC_EQ_FILTER_LEN];
} iir_filter, * IIR_FILTER;

typedef struct _fir_filter {
    float * taps;
    uint32   length;
    float  * buffer;
    uint32   buf_len;
    uint32   pointer;
} fir_filter, * FIR_FILTER ;

/* Used to hold state information for a GMSK Demodulator object */
typedef struct _gmsk_demod {
    uint32  m_pll;
    BOOL    m_prev;
    BOOL    m_invert;
} gmsk_demod, * GMSK_DEMOD;

typedef struct _gmsk_mod {
    BOOL m_invert;

    FIR_FILTER filter;
    IIR_FILTER iir;
} gmsk_mod, * GMSK_MOD;

void gmsk_testBitsAndEncodeDecode( void );

void gmsk_bitsToByte( BOOL * bits, unsigned char * byte );
void gmsk_bitsToBytes( BOOL * bits, unsigned char * bytes, uint32 num_of_bits );
void gmsk_byteToBits( unsigned char byte, BOOL * bits, uint32 num_bits );
void gmsk_bytesToBits( unsigned char * bytes, BOOL * bits, uint32 num_bits );

BOOL gmsk_encodeBuffer( GMSK_MOD mod, unsigned char * bytes, uint32 num_bits, float * buffer, uint32 buf_len );
void gmsk_decodeBuffer( GMSK_DEMOD demod, float * buffer, uint32 buf_len, unsigned char * bytes, uint32 num_bits );

void gmsk_resetMODFilter( GMSK_MOD mod );

uint32 gmsk_encode( GMSK_MOD mod, BOOL bit, float * buffer, unsigned int length );
enum DEMOD_STATE gmsk_decode( GMSK_DEMOD demod, float val );

void gmskDemod_reset( GMSK_DEMOD demod );

GMSK_MOD gmsk_createModulator( void );
GMSK_DEMOD gmsk_createDemodulator( void );
void gmsk_destroyModulator( GMSK_MOD mod );
void gmsk_destroyDemodulator( GMSK_DEMOD demod );

#endif /* THUMBDV_GMSK_MODEM_H_ */
