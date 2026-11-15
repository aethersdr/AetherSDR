///*!   \file gmsk_modem.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "gmsk_modem.h"
#include "bit_pattern_matcher.h"

/* Filters */

void gmsk_bitsToByte( BOOL * bits, unsigned char * byte ) {
    if ( bits == NULL || byte == NULL ) {
        output( ANSI_RED "NULL Pointer in bitsToByte\n" ANSI_WHITE );
        return;
    }

    unsigned char new_byte = 0x0;
    uint32 i = 0;

    for ( i = 0 ; i < 8; i++ ) {
        new_byte <<= 1;

        if ( bits[i] ) {
            new_byte |= 0x01;
        }
    }

    *byte = new_byte;
}

void gmsk_bitsToBytes( BOOL * bits, unsigned char * bytes, uint32 num_of_bits ) {
    if ( bits == NULL || bytes == NULL ) {
        output( ANSI_RED "NULL Pointer in bitsToBytes\n" ANSI_WHITE );
        return;
    }

    uint32 i = 0;

    for ( i = 0 ; i < num_of_bits / 8 ; i++ ) {
        gmsk_bitsToByte( &bits[i * 8], &bytes[i] );
    }

}

void gmsk_byteToBits( unsigned char byte, BOOL * bits, uint32 num_bits ) {
    if ( bits == NULL ) {
        output( ANSI_RED "NULL Pointer in byteToBits\n" ANSI_WHITE );
        return;
    }

    uint32 i = 0;
    unsigned char mask = 0x80;

    for ( i = 0; i < num_bits; i++ , mask >>= 1 ) {
        bits[i] = ( byte & mask ) ? TRUE : FALSE;
    }
}

void gmsk_bytesToBits( unsigned char * bytes, BOOL * bits, uint32 num_bits ) {
    if ( bytes == NULL || bits == NULL ) {
        output( ANSI_RED "NULL pointers in bytesToBits\n" ANSI_WHITE );
        return;
    }

    int32 bits_left = num_bits;
    uint32 byte_idx = 0;

    while ( bits_left > 0 ) {
        gmsk_byteToBits( bytes[byte_idx], &bits[byte_idx * 8], bits_left > 8 ? 8 : bits_left );
        byte_idx++;
        bits_left -= 8;
    }

//
//    uint32 i = 0;
//    output("Bytes: ");
//    for ( i = 0 ; i < num_bits / 8U ; i++ ) {
//        output(" 0x%02X", bytes[i]);
//    }
//    output("\nBits: ");
//    for ( i = 0 ; i < num_bits ; i++ ) {
//        output("%s ", bits[i] ? "1":"0");
//        if ( (i+1) % 4 == 0 ) {
//            output("   ");
//        }
//    }
//    output("\n");

}

float gmsk_FilterProcessSingle( FIR_FILTER filter, float val ) {
    if ( filter == NULL ) {
        output( ANSI_RED "NULL FIlter object\n"ANSI_WHITE ) ;
        return val;
    }

    float * ptr = filter->buffer + filter->pointer++;

    *ptr = val;

    float * a = ptr - filter->length + 1U;
    float * b = filter->taps;

    float out = 0.0F;
    uint32 i = 0;

    for ( i = 0U; i < filter->length; i++ )
        out += ( *a++ ) * ( *b++ );

    if ( filter->pointer == filter->buf_len ) {
        memcpy( filter->buffer, filter->buffer + filter->buf_len - filter->length, filter->length * sizeof( float ) );
        filter->pointer = filter->length;
    }

    return out;
}

void gmsk_FilterProcessBuffer( FIR_FILTER filter, float * buffer, uint32 buffer_len ) {
    if ( filter == NULL ) {
        output( ANSI_RED "NULL FIlter object\n"ANSI_WHITE ) ;
        return;
    }

    uint32 i = 0;

    for ( i = 0 ; i < buffer_len; i++ ) {
        buffer[i] = gmsk_FilterProcessSingle( filter, buffer[i] );
    }
}

/* Demod Section */

#define PLLMAX  0x10000U
#define PLLINC      ( PLLMAX / GMSK_SAMPLES_PER_SYMBOL)
#define INC  32U

enum DEMOD_STATE gmsk_decode( GMSK_DEMOD demod, float val ) {
    enum DEMOD_STATE state = DEMOD_UNKNOWN;

    /* FIlter process */
    float out = val;//gmsk_FilterProcessSingle(demod->filter, val);

    BOOL bit = out > 0.0F;

    if ( bit != demod->m_prev ) {
        if ( demod->m_pll < ( PLLMAX / 2U ) ) {
            demod->m_pll += PLLINC / INC;
        } else {
            demod->m_pll -= PLLINC / INC;
        }
    }

    demod->m_prev = bit;
    demod->m_pll += PLLINC;

    if ( demod->m_pll >= PLLMAX ) {
        if ( demod->m_invert )
            state = bit ? DEMOD_TRUE : DEMOD_FALSE;
        else
            state = bit ? DEMOD_FALSE : DEMOD_TRUE;

        demod->m_pll -= PLLMAX;
    }

    return state;
}

void gmskDemod_reset( GMSK_DEMOD demod ) {
    demod->m_pll  = 0U;
    demod->m_prev = FALSE;
}

void gmsk_decodeBuffer( GMSK_DEMOD demod, float * buffer, uint32 buf_len, unsigned char * bytes, uint32 num_bits ) {
    if ( num_bits * GMSK_SAMPLES_PER_SYMBOL != buf_len ) {
        output( ANSI_RED "Mismatched buf_len to number of encoded bits. buf_len = %d, required %d\n" ANSI_WHITE, buf_len, num_bits * GMSK_SAMPLES_PER_SYMBOL );
        return;
    }

    BOOL * bits = ( BOOL * )calloc( num_bits, sizeof( BOOL ) );
    if ( bits == NULL ) {
        output( ANSI_RED "Unable to allocate GMSK decode bit buffer\n" ANSI_WHITE );
        return;
    }
    uint32 i = 0;
    uint32 bit = 0;
    enum DEMOD_STATE state;

    for ( i = 0; i < buf_len ; i++ ) {
        state = gmsk_decode( demod, buffer[i] );

        if ( state == DEMOD_TRUE ) {
            bits[bit] = TRUE;
            bit++;
        } else if ( state == DEMOD_FALSE ) {
            bits[bit] = FALSE;
            bit++;
        } else {
            //output("UNKNOWN DEMOD STATE");
            //bits[bit] = 0x00;
        }
    }

    for ( i = 0; i < bit; i++ ) {
        output( "%d", bits[i] ? 1 : 0 );

        if ( ( i + 1 ) % 4 == 0 ) output( "  " );
    }

    output( "\n" );

//    FILE * f = fopen("gmsk_demod.dat", "w");
//    for ( i = 0 ; i < num_bits ; i++ ) {
//        fprintf(f,"%d %d\n", i, bits[i]);
//    }
//    fclose(f);

    for ( i = 0 ; i < num_bits / 8 ; i++ ) {
        gmsk_bitsToByte( &bits[i * 8], &bytes[i] );
    }
    free( bits );
}

/* Mod Section */


/*
 * Five-sample/symbol BT 0.35 Gaussian interpolation filter. The coefficients
 * are the Q15 gaussfir(0.35, 1, 5) reference used by the GPLv2-or-later MMDVM
 * D-STAR modem, converted to float. Its 24 kHz / 4.8 ksym/s operating point
 * matches the Flex waveform stream exactly.
 */
static const float MOD_COEFFS_TABLE[] = {
    0.0f,                    0.0f,                    0.0f,
    0.0f,                    1001.0f / 32768.0f,     3514.0f / 32768.0f,
    9333.0f / 32768.0f,     18751.0f / 32768.0f,    28499.0f / 32768.0f,
    32767.0f / 32768.0f,    28499.0f / 32768.0f,    18751.0f / 32768.0f,
    9333.0f / 32768.0f,     3514.0f / 32768.0f,     1001.0f / 32768.0f
};

enum { MOD_COEFFS_LENGTH = 15U };
static const float MOD_SYMBOL_LEVEL = 0.9f;

uint32 gmsk_encode( GMSK_MOD mod, BOOL bit, float * buffer, unsigned int length ) {

    if ( length != GMSK_SAMPLES_PER_SYMBOL ) {
        output( ANSI_RED "Length!= GMSK_SAMPLES_PER_SYMBOL" ANSI_WHITE );
    }

    if ( mod->m_invert ) {
        bit = !bit;
    }

    const float symbol = bit ? -MOD_SYMBOL_LEVEL : MOD_SYMBOL_LEVEL;
    uint32 i = 0U;

    for ( i = 0U; i < GMSK_SAMPLES_PER_SYMBOL; i++ ) {
        const float input = i == 0U ? symbol : 0.0f;
        buffer[i] = gmsk_FilterProcessSingle( mod->filter, input );
    }

    return GMSK_SAMPLES_PER_SYMBOL;
}

BOOL gmsk_encodeBuffer( GMSK_MOD mod, unsigned char * bytes, uint32 num_bits, float * buffer, uint32 buf_len ) {
    if ( num_bits * GMSK_SAMPLES_PER_SYMBOL != buf_len ) {
        output( ANSI_RED "Mismatched buf_len to number of encoded bits. buf_len = %d, required %d\n" ANSI_WHITE, buf_len, num_bits * GMSK_SAMPLES_PER_SYMBOL );
        return FALSE;
    }

    uint32 i = 0;
    float * idx = &buffer[0];

    BOOL * bits = ( BOOL * )calloc( num_bits, sizeof( BOOL ) );
    if ( bits == NULL ) {
        output( ANSI_RED "Unable to allocate GMSK encode bit buffer\n" ANSI_WHITE );
        return FALSE;
    }

    gmsk_bytesToBits( bytes, bits, num_bits );

    for ( i = 0 ; i < num_bits ; i++, idx += GMSK_SAMPLES_PER_SYMBOL ) {
        gmsk_encode( mod, bits[i], idx, GMSK_SAMPLES_PER_SYMBOL );
    }

    free( bits );
    return TRUE;
}


/* Init */

void gmsk_resetMODFilter( GMSK_MOD mod ) {
    if ( mod == NULL || mod->filter == NULL ) {
        return;
    }
    memset( mod->filter->buffer, 0, mod->filter->buf_len * sizeof( float ) );
    mod->filter->pointer = mod->filter->length;
}

FIR_FILTER gmsk_createFilter( const float * taps, uint32 length ) {
    FIR_FILTER filter = ( FIR_FILTER ) safe_malloc( sizeof( fir_filter ) );
    memset( filter, 0, sizeof( fir_filter ) );

    filter->length = length;
    filter->buf_len = 20 * length;
    filter->taps = ( float * ) safe_malloc( length * sizeof( float ) );
    memcpy( filter->taps, taps, length * sizeof( float ) );

    filter->buffer = ( float * ) safe_malloc( filter->buf_len * sizeof( float ) );
    memset( filter->buffer, 0, filter->buf_len * sizeof( float ) );
    filter->pointer = length;

    return filter;
}

void gmsk_destroyFilter( FIR_FILTER filter ) {
    if ( filter == NULL ) {
        output( ANSI_RED "NULL FIlter object\n"ANSI_WHITE ) ;
        return;
    }

    safe_free( filter->taps );
    safe_free( filter->buffer );
    safe_free( filter );

}

GMSK_DEMOD gmsk_createDemodulator( void ) {
    GMSK_DEMOD demod = ( GMSK_DEMOD ) safe_malloc( sizeof( gmsk_demod ) );
    memset( demod, 0, sizeof( gmsk_demod ) );
    demod->m_invert = FALSE;
    gmskDemod_reset( demod );

    return demod;
}

GMSK_MOD gmsk_createModulator( void ) {
    GMSK_MOD mod = ( GMSK_MOD ) safe_malloc( sizeof( gmsk_mod ) );
    memset( mod, 0, sizeof( gmsk_mod ) );
    mod->m_invert = FALSE;

    mod->filter = gmsk_createFilter( MOD_COEFFS_TABLE, MOD_COEFFS_LENGTH );

    return mod;
}

void gmsk_destroyDemodulator( GMSK_DEMOD demod ) {
    if ( demod == NULL ) {
        output( ANSI_RED "NULL GMSK_DEMOD\n" ANSI_WHITE );
        return;
    }

    safe_free( demod );
}

void gmsk_destroyModulator( GMSK_MOD mod ) {

    if ( mod == NULL ) {
        output( ANSI_RED "NULL GMSK_MOD\n" ANSI_WHITE );
        return;
    }

    gmsk_destroyFilter( mod->filter );
    safe_free( mod );

}

void gmsk_testBitsAndEncodeDecode( void ) {
    GMSK_DEMOD _gmsk_demod = gmsk_createDemodulator();
    GMSK_MOD _gmsk_mod = gmsk_createModulator();

    unsigned char pattern[1] = {0xAA};
    BOOL pattern_bits[8] = {0};
    gmsk_bytesToBits( pattern, pattern_bits, 8 );

    BIT_PM _bit_pm  = bitPM_create( pattern_bits, 8 );

    float test_buffer[160 * 2];
    unsigned char test_coded[8] =  {0xAA, 0xAA, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    unsigned char output_bytes[8] = {0};
    uint32 i = 0;


    BOOL bits[64] = {0};
    gmsk_bytesToBits( test_coded, bits, 32 );
    gmsk_byteToBits( 0xF0, bits, 8 );
    output( "0xF0 = " );

    for ( i = 0 ; i < 8; i++ ) {
        output( "%d ", bits[i] );
    }

    output( "\n" );
    unsigned char test[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    gmsk_bytesToBits( test, bits, 32 );

    for ( i = 0 ; i < 32 / 8 ; i++ ) {
        gmsk_bitsToByte( &bits[i * 8], &output_bytes[i] );
        output( "Byte = 0x%02X\n", output_bytes[i] );
    }

    gmsk_encodeBuffer( _gmsk_mod, test_coded, 32 * 2, test_buffer, 160 * 2 );
    FILE * dat = fopen( "gmsk.dat", "w" );

    for ( i = 0 ; i < 160 * 2 ; i++ ) {
        fprintf( dat, "%d %.12f\n", i, test_buffer[i] );
        //output("%.12f,", test_buffer[i]);
    }

    fclose( dat );

    gmsk_decodeBuffer( _gmsk_demod, test_buffer, 160 * 2, output_bytes, 32 * 2 );

    gmsk_bytesToBits( output_bytes, bits, 32 * 2 );
    output( "STARTING PATTERN MATCH TEST \n" );

    for ( i = 0 ; i < 32 * 2; i++ ) {
        output( "%d ", bits[i] );

        if ( bitPM_addBit( _bit_pm, bits[i] ) ) {
            output( "MATCH!\n" );
            bitPM_reset( _bit_pm );
        }

    }

    bitPM_destroy( _bit_pm );
    gmsk_destroyDemodulator( _gmsk_demod );
    gmsk_destroyModulator( _gmsk_mod );

}
