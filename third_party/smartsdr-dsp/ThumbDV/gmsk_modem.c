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
#include "DStarDefines.h"
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

    float * a = ptr - filter->length;
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
#define PLLINC      ( PLLMAX / DSTAR_RADIO_BIT_LENGTH) // 2000
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
    if ( num_bits * DSTAR_RADIO_BIT_LENGTH != buf_len ) {
        output( ANSI_RED "Mismatched buf_len to number of encoded bits. buf_len = %d, required %d\n" ANSI_WHITE, buf_len, num_bits * DSTAR_RADIO_BIT_LENGTH );
        return;
    }

    BOOL bits[num_bits];
    memset( bits, 0, num_bits * sizeof( BOOL ) );
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
}

/* Mod Section */


// Generated by
// gaussfir(0.5, 4, 10)
const float MOD_COEFFS_TABLE[] = {
    1.01839713019626E-50, 6.78135172681677E-46, 2.55482745053591E-41,
    5.44569202979357E-37, 6.56735934398859E-33, 4.48099730728277E-29,
    1.72983542063190E-25, 3.77816645793944E-22, 4.66878194148063E-19,
    3.26416641874238E-16, 1.29118120144693E-13, 2.88967031267069E-11,
    3.65894261059669E-09, 2.62125395740380E-07, 1.06245073266054E-05,
    2.43643428039816E-04, 3.16116236025481E-03, 2.32051886177988E-02,
    9.63761566612470E-02, 2.26464589054118E-01, 3.01076739115701E-01,
    2.26464589054118E-01, 9.63761566612468E-02, 2.32051886177989E-02,
    3.16116236025481E-03, 2.43643428039816E-04, 1.06245073266054E-05,
    2.62125395740378E-07, 3.65894261059672E-09, 2.88967031267069E-11,
    1.29118120144693E-13, 3.26416641874238E-16, 4.66878194148056E-19,
    3.77816645793952E-22, 1.72983542063190E-25, 4.48099730728277E-29,
    6.56735934398859E-33, 5.44569202979341E-37, 2.55482745053598E-41,
    6.78135172681677E-46, 1.01839713019626E-50
};

#define MOD_COEFFS_LENGTH 41U

#define eiline(n) (iir->in_line[(n)])
#define eoline(n) (iir->out_line[(n)])

static float _process_iir_filter(IIR_FILTER iir, float sample)
{

    float * a = iir->a;
    float * b = iir->b;
    float N = 16.0f; // Scaling value for IIR Filter Realization
    float N_IV = 1.0f / N;
    float N_2 = N / 2.0f;

    float local_out;
    //Shift Input Line
    eiline( 2) = eiline( 1);
    eiline( 1) = eiline( 0);

    eiline( 0) = sample;

    eoline( 2) = eoline( 1);
    eoline( 1) = eoline( 0);

    local_out = 0.0f;


    local_out = 2.0f * ((-a[1]*0.5f * eoline(
                                1)) - (a[2]*0.5f * eoline( 2))
                               + N_2 * ((b[0]*N_IV * eiline( 0)) + (b[1]*N_IV
                                       * eiline( 1)) + (b[2]*N_IV * eiline( 2))));

    eoline(0) = local_out;
    return local_out;
}

uint32 gmsk_encode( GMSK_MOD mod, BOOL bit, float * buffer, unsigned int length ) {

    if ( length != DSTAR_RADIO_BIT_LENGTH ) {
        output( ANSI_RED "Length!= DSTAR_RADIO_BIT_LENGTH" ANSI_WHITE );
    }

    if ( mod->m_invert )
        bit = !bit;

    uint32 i = 0;

    for ( i = 0; i < DSTAR_RADIO_BIT_LENGTH; i++ ) {
        if ( bit ) {
            buffer[i] = gmsk_FilterProcessSingle( mod->filter, -0.75f );
        } else {
            buffer[i] = gmsk_FilterProcessSingle( mod->filter, 0.75f );
        }

        buffer[i] = _process_iir_filter(mod->iir, buffer[i]);

    }

    return DSTAR_RADIO_BIT_LENGTH;
}

BOOL gmsk_encodeBuffer( GMSK_MOD mod, unsigned char * bytes, uint32 num_bits, float * buffer, uint32 buf_len ) {
    if ( num_bits * DSTAR_RADIO_BIT_LENGTH != buf_len ) {
        output( ANSI_RED "Mismatched buf_len to number of encoded bits. buf_len = %d, required %d\n" ANSI_WHITE, buf_len, num_bits * DSTAR_RADIO_BIT_LENGTH );
        return FALSE;
    }

    uint32 i = 0;
    float * idx = &buffer[0];

    BOOL bits[num_bits];
    memset( bits, 0, num_bits * sizeof( BOOL ) );

    gmsk_bytesToBits( bytes, bits, num_bits );

    for ( i = 0 ; i < num_bits ; i++, idx += DSTAR_RADIO_BIT_LENGTH ) {
        gmsk_encode( mod, bits[i], idx, DSTAR_RADIO_BIT_LENGTH );
    }

    return TRUE;
}


/* Init */

void gmsk_resetMODFilter( GMSK_MOD mod ) {
    memset( mod->filter->buffer, 0, mod->filter->buf_len * sizeof( float ) );
    mod->filter->pointer = mod->filter->length;
}

FIR_FILTER gmsk_createFilter( const float * taps, uint32 length ) {
    FIR_FILTER filter = ( FIR_FILTER ) safe_malloc( sizeof( fir_filter ) );

    filter->length = length;
    filter->buf_len = 20 * length;
    filter->taps = ( float * ) safe_malloc( length * sizeof( float ) );
    memcpy( filter->taps, taps, length * sizeof( float ) );

    filter->buffer = ( float * ) safe_malloc( filter->buf_len * sizeof( float ) );
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
    demod->m_pll = 0;
    demod->m_prev = FALSE;
    demod->m_invert = FALSE;

    return demod;
}


float convertVITAdbToFloat(VITAdb vita)
{
    int32 db = (int32)((int16)(vita & 0xFFFF));
    return (float)db / 128.0;
}
// always assume the float is in dB (see the parameter name)
VITAdb convertFloatToVITAdb(float db)
{
    return ((int32)(db * 128) & 0xFFFF);
}

GMSK_MOD gmsk_createModulator( void ) {
    GMSK_MOD mod = ( GMSK_MOD ) safe_malloc( sizeof( gmsk_mod ) );
    mod->m_invert = FALSE;

    mod->filter = gmsk_createFilter( MOD_COEFFS_TABLE, MOD_COEFFS_LENGTH );


    mod->iir = safe_malloc(sizeof(iir_filter));

    IIR_FILTER filter = mod->iir;

    float * a = filter->a;
    float * b = filter->b;
    uint32 freq_hz = 4800;
    uint32 sample_rate_hz = 24000;
    uint32 level = 10;
    float q_level = 0.2;
        float w0 = 2.0 * M_PI * freq_hz/
                                      (float)sample_rate_hz;
        VITAdb db_gain = convertFloatToVITAdb(level);
        float q_factor;
        if ( level > 0 ) {
            q_factor = q_level;
        } else {
            q_factor = 4.0f * q_level;
        }
        float A = pow(10, convertVITAdbToFloat(db_gain) / 40);
        float alpha = sin(w0) / (2 * A * q_factor);
        //flex_printf(LOG_ALWAYS, "w0 = %lf, A = %lf, alpha = %lf", w0, A, alpha);

        //float linearGain = pow(10,  convertVITAdbToFloat(db_gain) / 20);

        if (level != 0 ) {
            // Peaking EQ from AUDIO COOKBOOK
            b[0] = 1 + alpha * A;
            b[1] = -2 * cos(w0);
            b[2] = 1 - alpha * A;

            a[0] = 1 + alpha/A;
            a[1] = -2 * cos(w0);
            a[2] = 1 - alpha / A;
        } else {
            b[0] = 0;
            b[1] = 0;
            b[2] = 0;
            a[0] = 1;
            a[1] = 0;
            a[2] = 0;
        }

        // Normalize by a[0] so that a[0] = 1
        b[0] = b[0]  / a[0];
        b[1] = b[1]  / a[0];
        b[2] = b[2]  / a[0];

        a[1] = a[1]  / a[0];
        a[2] = a[2]  / a[0];
        a[0] = a[0]  / a[0];


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
    safe_free(mod->iir);
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
