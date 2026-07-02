///*!   \file sched_waveform.c
// *    \brief Schedule Wavefrom Streams
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 29-AUG-2014
// *    \author 	Ed Gonzalez
// *    \mangler 	Graham / KE9H
// *
// */

/* *****************************************************************************
 *
 *  Copyright (C) 2014 FlexRadio Systems.
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

#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>		// for memset
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/prctl.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"
#include "sched_waveform.h"
#include "vita_output.h"
#include "aether_vocoder_backend.h"
#include "bit_pattern_matcher.h"
#include "dstar.h"
#include "DStarDefines.h"
#include "slow_data.h"
#include "ftd2xx.h"

//static Queue sched_fft_queue;
static pthread_rwlock_t _list_lock;
static BufferDescriptor _root;

static pthread_t _waveform_thread;
static BOOL _waveform_thread_abort = FALSE;

static sem_t sched_waveform_sem;

static void _dsp_convertBufEndian( BufferDescriptor buf_desc ) {
    int i;

    if ( buf_desc->sample_size != 8 ) {
        //TODO: horrendous error here
        return;
    }

    for ( i = 0; i < buf_desc->num_samples * 2; i++ )
        ( ( int32 * )buf_desc->buf_ptr )[i] = htonl( ( ( int32 * )buf_desc->buf_ptr )[i] );
}

static BufferDescriptor _WaveformList_UnlinkHead( void ) {
    BufferDescriptor buf_desc = NULL;
    pthread_rwlock_wrlock( &_list_lock );

    if ( _root == NULL || _root->next == NULL ) {
        output( "Attempt to unlink from a NULL head" );
        pthread_rwlock_unlock( &_list_lock );
        return NULL;
    }

    if ( _root->next != _root )
        buf_desc = _root->next;

    if ( buf_desc != NULL ) {
        // make sure buffer exists and is actually linked
        if ( !buf_desc || !buf_desc->prev || !buf_desc->next ) {
            output( "Invalid buffer descriptor" );
            buf_desc = NULL;
        } else {
            buf_desc->next->prev = buf_desc->prev;
            buf_desc->prev->next = buf_desc->next;
            buf_desc->next = NULL;
            buf_desc->prev = NULL;
        }
    }

    pthread_rwlock_unlock( &_list_lock );
    return buf_desc;
}

static void _WaveformList_LinkTail( BufferDescriptor buf_desc ) {
    pthread_rwlock_wrlock( &_list_lock );
    buf_desc->next = _root;
    buf_desc->prev = _root->prev;
    _root->prev->next = buf_desc;
    _root->prev = buf_desc;
    pthread_rwlock_unlock( &_list_lock );
}

void sched_waveform_Schedule( BufferDescriptor buf_desc ) {
    _WaveformList_LinkTail( buf_desc );
    sem_post( &sched_waveform_sem );
}

void sched_waveform_signal() {
    sem_post( &sched_waveform_sem );
}

static void _dstar_copy_padded_field( char * destination, uint32 field_len, const char * source )
{
    if ( destination == NULL || field_len == 0U ) {
        return;
    }

    memset( destination, ' ', field_len );
    destination[field_len] = '\0';

    if ( source == NULL ) {
        return;
    }

    uint32 i = 0;
    for ( i = 0; i < field_len && source[i] != '\0'; i++ ) {
        destination[i] = ( ( unsigned char )source[i] == 0x7FU ) ? ' ' : source[i];
    }
}

/* *********************************************************************************************
 * *********************************************************************************************
 * *********************                                                 ***********************
 * *********************  LOCATION OF MODULATOR / DEMODULATOR INTERFACE  ***********************
 * *********************                                                 ***********************
 * *********************************************************************************************
 * ****************************************************************************************** */

#include <stdio.h>
#include "circular_buffer.h"
#include "resampler.h"

#include "gmsk_modem.h"

#define PACKET_SAMPLES  128
#define DV_PACKET_SAMPLES 160

#define SCALE_AMBE      32767.0f
//
//#define SCALE_RX_IN      32767.0f   // Multiplier   // Was 16000 GGH Jan 30, 2015
//#define SCALE_RX_OUT     32767.0f       // Divisor
//#define SCALE_TX_IN     32767.0f    // Multiplier   // Was 16000 GGH Jan 30, 2015
//#define SCALE_TX_OUT    32767.0f    // Divisor

#define SCALE_RX_IN     SCALE_AMBE*2.0
#define SCALE_TX_OUT    SCALE_AMBE


#define SCALE_RX_OUT    SCALE_AMBE*2.0
#define SCALE_TX_IN     SCALE_AMBE


#define FILTER_TAPS	48
#define DECIMATION_FACTOR 	3

/* These are offsets for the input buffers to decimator */
#define MEM_24		FILTER_TAPS					   /* Memory required in 24kHz buffer */
#define MEM_8		FILTER_TAPS/DECIMATION_FACTOR   /* Memory required in 8kHz buffer */

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Circular Buffer Declarations

short RX3_buff[( DV_PACKET_SAMPLES * 12 ) + 1];		// RX3 Vocoder output buffer
float RX4_buff[( DV_PACKET_SAMPLES * 12 * 40 ) + 1];		// RX4 Packet output Buffer

float TX1_buff[( DV_PACKET_SAMPLES * 12 ) + 1];		// TX1 Packet Input Buffer
short TX2_buff[( DV_PACKET_SAMPLES * 12 ) + 1];		// TX2 Vocoder input buffer
short TX3_buff[( DV_PACKET_SAMPLES * 12 ) + 1];		// TX3 Vocoder output buffer
float TX4_buff[( DV_PACKET_SAMPLES * 12 * 40 ) + 1];		// TX4 Packet output Buffer

circular_short_buffer rx3_cb;
Circular_Short_Buffer RX3_cb = &rx3_cb;
circular_float_buffer rx4_cb;
Circular_Float_Buffer RX4_cb = &rx4_cb;

circular_float_buffer tx1_cb;
Circular_Float_Buffer TX1_cb = &tx1_cb;
circular_short_buffer tx2_cb;
Circular_Short_Buffer TX2_cb = &tx2_cb;
circular_short_buffer tx3_cb;
Circular_Short_Buffer TX3_cb = &tx3_cb;
circular_float_buffer tx4_cb;
Circular_Float_Buffer TX4_cb = &tx4_cb;

static FT_HANDLE _dv_serial_handle = 0;

static GMSK_DEMOD _gmsk_demod = NULL;
static GMSK_MOD   _gmsk_mod = NULL;
static DSTAR_MACHINE _dstar = NULL;
static BOOL _end_of_transmission = FALSE;

#define FREEDV_NSAMPLES 160

void sched_waveform_sendStatus( uint32 slice ) {
    dstar_updateStatus( _dstar, slice, STATUS_TX );
}

void sched_waveform_setDestinationRptr( uint32 slice , const char * destination_rptr ) {
    /* Ignore slice for now */
    if ( _dstar == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->outgoing_header.destination_rptr, 8, destination_rptr );

    if ( strncmp( _dstar->outgoing_header.destination_rptr, "DIRECT", strlen( "DIRECT" ) ) != 0 ) {
        _dstar->outgoing_header.flag1 = 0x1 << 6;
    } else {
        _dstar->outgoing_header.flag1 = 0;
    }

    dstar_dumpHeader( &( _dstar->outgoing_header ) );
}

void sched_waveform_setDepartureRptr( uint32 slice , const char * departure_rptr ) {
    /* Ignore slice for now */
    if ( _dstar == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->outgoing_header.departure_rptr, 8, departure_rptr );

    dstar_dumpHeader( &( _dstar->outgoing_header ) );
}

void sched_waveform_setCompanionCall( uint32 slice, const char * companion_call ) {
    /* Ignore slice for now */
    if ( _dstar == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->outgoing_header.companion_call, 8, companion_call );

    dstar_dumpHeader( &( _dstar->outgoing_header ) );
}

void sched_waveform_setOwnCall1( uint32 slice , const char * owncall1 ) {
    /* Ignore slice for now */
    if ( _dstar == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->outgoing_header.own_call1, 8, owncall1 );

    dstar_dumpHeader( &( _dstar->outgoing_header ) );
}

void sched_waveform_setOwnCall2( uint32 slice , const char * owncall2 ) {
    /* Ignore slice for now */
    if ( _dstar == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->outgoing_header.own_call2, 4, owncall2 );

    dstar_dumpHeader( &( _dstar->outgoing_header ) );
}

void sched_waveform_setMessage( uint32 slice, const char * message)
{
    /* Ignore slice for now */
    if ( _dstar == NULL || _dstar->slow_encoder == NULL ) {
        return;
    }

    _dstar_copy_padded_field( _dstar->slow_encoder->message,
                              SLOW_DATA_MESSAGE_LENGTH_BYTES,
                              message );

    output( "TX Message: '%s' : strlen() = %zu \n", _dstar->slow_encoder->message , strlen(_dstar->slow_encoder->message));
}

void sched_waveform_setHandle( FT_HANDLE * handle ) {
    _dv_serial_handle = *handle;
}

void sched_waveform_setEndOfTX( BOOL end_of_transmission ) {
    _end_of_transmission = TRUE;
}

void sched_waveform_setDSTARSlice( uint32 slice )
{
    if ( _dstar != NULL ) {
        _dstar->slice = slice;
    }
}

static void * _sched_waveform_thread( void * param ) {

    prctl(PR_SET_NAME, "DV-SchedWav");
    int 	nout;

    int		i;			// for loop counter
    float	fsample;	// a float sample
//    float   Sig2Noise;	// Signal to noise ratio

    // Flags ...
    int		initial_tx = TRUE; 		// Flags for TX circular buffer, clear if starting transmit
    int		initial_rx = TRUE;			// Flags for RX circular buffer, clear if starting receive

    // VOCODER I/O BUFFERS
    short	speech_in[DV_PACKET_SAMPLES];
    short 	speech_out[DV_PACKET_SAMPLES];
    //short 	demod_in[FREEDV_NSAMPLES];
    unsigned char 	mod_out[DV_PACKET_SAMPLES];

    //unsigned char packet_out[FREEDV_NSAMPLES];

    // RX RESAMPLER I/O BUFFERS
    float 	float_in_8k[DV_PACKET_SAMPLES + FILTER_TAPS];
    //float 	float_out_8k[DV_PACKET_SAMPLES];

    float 	float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];
    float 	float_out_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR ];

    // TX RESAMPLER I/O BUFFERS
    float 	tx_float_in_8k[DV_PACKET_SAMPLES + FILTER_TAPS];
    float 	tx_float_out_8k[DV_PACKET_SAMPLES];

    float 	tx_float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];

    BOOL inhibit_tx = FALSE;
    BOOL flush_tx = FALSE;

    // =======================  Initialization Section =========================

    aether_vocoder_init( &_dv_serial_handle );

    // Initialize the Circular Buffers

    RX3_cb->size  = DV_PACKET_SAMPLES * 12 + 1;		// size = no.elements in array+1
    RX3_cb->start = 0;
    RX3_cb->end	  = 0;
    RX3_cb->elems = RX3_buff;
    strncpy( RX3_cb->name, "RX3", 4 );

    RX4_cb->size  = DV_PACKET_SAMPLES * ( 12 * 40 ) + 1;		// size = no.elements in array+1
    RX4_cb->start = 0;
    RX4_cb->end	  = 0;
    RX4_cb->elems = RX4_buff;
    strncpy( RX4_cb->name, "RX4", 4 );

    TX1_cb->size  = DV_PACKET_SAMPLES * 12 + 1;		// size = no.elements in array+1
    TX1_cb->start = 0;
    TX1_cb->end	  = 0;
    TX1_cb->elems = TX1_buff;
    strncpy( TX1_cb->name, "TX1", 4 );

    TX2_cb->size  = DV_PACKET_SAMPLES * 12 + 1;		// size = no.elements in array+1
    TX2_cb->start = 0;
    TX2_cb->end	  = 0;
    TX2_cb->elems = TX2_buff;
    strncpy( TX2_cb->name, "TX2", 4 );

    TX3_cb->size  = DV_PACKET_SAMPLES * 12 + 1;		// size = no.elements in array+1
    TX3_cb->start = 0;
    TX3_cb->end	  = 0;
    TX3_cb->elems = TX3_buff;
    strncpy( TX3_cb->name, "TX3", 4 );

    TX4_cb->size  = DV_PACKET_SAMPLES * ( 12 * 40 ) + 1;		// size = no.elements in array+1
    TX4_cb->start = 0;
    TX4_cb->end	  = 0;
    TX4_cb->elems = TX4_buff;
    strncpy( TX4_cb->name, "TX4", 4 );

    initial_tx = TRUE;
    initial_rx = TRUE;
    BOOL initial_tx_flush = FALSE;
    uint32 dstar_tx_frame_count = 0;

    // show that we are running
    BufferDescriptor buf_desc;

    while ( !_waveform_thread_abort ) {
        // wait for a buffer descriptor to get posted
        sem_wait( &sched_waveform_sem );

        if ( !_waveform_thread_abort ) {
            do {
                buf_desc = _WaveformList_UnlinkHead();

                // if we got signalled, but there was no new data, something's wrong
                // and we'll just wait for the next packet
                if ( buf_desc == NULL ) {
                    //output( "We were signaled that there was another buffer descriptor, but there's not one here");
                    break;
                } else {
                    // convert the buffer to little endian
                    _dsp_convertBufEndian( buf_desc );

                    //output(" \"Processed\" buffer stream id = 0x%08X\n", buf_desc->stream_id);

                    if ( ( buf_desc->stream_id & 1 ) == 0 ) { //RX BUFFER
                        //	If 'initial_rx' flag, clear buffers RX1, RX2, RX3, RX4
                        if ( initial_rx ) {
                            RX3_cb->start = 0;
                            RX3_cb->end	  = 0;
                            RX4_cb->start = 0;
                            RX4_cb->end	  = 0;


                            /* Clear filter memory */
                            memset( float_in_24k, 0, MEM_24 * sizeof( float ) );
                            memset( float_in_8k, 0, MEM_8 * sizeof( float ) );

                            aether_vocoder_flush_lists();
                            /* Requires us to set initial_rx to FALSE which we do at the end of
                             * the first loop
                             */
                        }

                        //	Set the transmit 'initial' flag
                        initial_tx = TRUE;
                        inhibit_tx = FALSE;
                        flush_tx = FALSE;
                        _end_of_transmission = FALSE;
                        gmsk_resetMODFilter( _gmsk_mod );

                        enum DEMOD_STATE state = DEMOD_UNKNOWN;

                        for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                            state = gmsk_decode( _gmsk_demod, ( ( Complex * )buf_desc->buf_ptr )[i].real );

                            unsigned char ambe_out[9] = {0};
                            BOOL ambe_packet_out = FALSE;

                            if ( state == DEMOD_TRUE ) {
                                ambe_packet_out = dstar_rxStateMachine( _dstar, TRUE, ambe_out, 9 );
                            } else if ( state == DEMOD_FALSE ) {
                                ambe_packet_out = dstar_rxStateMachine( _dstar, FALSE, ambe_out, 9 );
                            } else {
                                /* Nothing to do since we have not "locked" a bit out yet */
                            }

                            if ( ambe_packet_out == TRUE ) {
                                aether_vocoder_decode( _dv_serial_handle, ambe_out, 9 );
                            }

                            if ( aether_vocoder_get_decode_list_buffering() == FALSE)
                            {
                                // There is something in the decoded list - fetch audio
                                nout = 0;
                                nout = aether_vocoder_unlink_audio(speech_out);
                                uint32 j = 0;

                                for ( j = 0 ; j < nout ; j++ )
                                {
                                    cbWriteShort( RX3_cb, speech_out[j] );
                                }
                            }

                        }


                        // Check for >= 160 samples in RX3_cb, convert to floats
                        //	and spin the upsampler. Move output to RX4_cb.

                        if ( csbContains( RX3_cb ) >= DV_PACKET_SAMPLES ) {
                            for ( i = 0 ; i < DV_PACKET_SAMPLES ; i++ ) {
                                float_in_8k[i + MEM_8] = ( ( float )( cbReadShort( RX3_cb ) / SCALE_RX_OUT ) );
                            }

                            fdmdv_8_to_24( float_out_24k, &float_in_8k[MEM_8], DV_PACKET_SAMPLES );

                            for ( i = 0 ; i < DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++ ) {
                                cbWriteFloat( RX4_cb, float_out_24k[i] );
                            }
                        }

                        // Check for >= 128 samples in RX4_cb. Form packet and
                        //	export.

                        uint32 check_samples = PACKET_SAMPLES;

                        if ( cfbContains( RX4_cb ) >= check_samples ) {
                            for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                // Set up the outbound packet
                                fsample = cbReadFloat( RX4_cb );
//								// put the fsample into the outbound packet

                                ( ( Complex * )buf_desc->buf_ptr )[i].real = fsample;
                                ( ( Complex * )buf_desc->buf_ptr )[i].imag = fsample;

                            }
                        } else {
                            memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof( Complex ) );
                        }

                        if ( initial_rx )
                            initial_rx = FALSE;

                        emit_waveform_output( buf_desc );

                    } else if ( ( buf_desc->stream_id & 1 ) == 1 ) { //TX BUFFER
                        //	If 'initial_rx' flag, clear buffers TX1, TX2, TX3, TX4
                        if ( initial_tx ) {
                            TX1_cb->start = 0;	// Clear buffers RX1, RX2, RX3, RX4
                            TX1_cb->end	  = 0;
                            TX2_cb->start = 0;
                            TX2_cb->end	  = 0;
                            TX3_cb->start = 0;
                            TX3_cb->end	  = 0;
                            TX4_cb->start = 0;
                            TX4_cb->end	  = 0;


                            /* Clear filter memory */

                            memset( tx_float_in_24k, 0, MEM_24 * sizeof( float ) );
                            memset( tx_float_in_8k, 0, MEM_8 * sizeof( float ) );

                            aether_vocoder_flush_lists();

                            /* Requires us to set initial_rx to FALSE which we do at the end of
                             * the first loop
                             */
                        }


                        initial_rx = TRUE;
                        // Check for new receiver input packet & move to TX1_cb.

                        if ( !inhibit_tx ) {


                            for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                //output("Outputting ")
                                //	fsample = Get next float from packet;
                                cbWriteFloat( TX1_cb, ( ( Complex * )buf_desc->buf_ptr )[i].real );

                            }

//
                            // Check for >= 384 samples in TX1_cb and spin downsampler
                            //	Convert to shorts and move to TX2_cb.
                            if ( cfbContains( TX1_cb ) >= DV_PACKET_SAMPLES * DECIMATION_FACTOR ) {
                                for ( i = 0 ; i < DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++ ) {
                                    tx_float_in_24k[i + MEM_24] = cbReadFloat( TX1_cb );
                                }

                                fdmdv_24_to_8( tx_float_out_8k, &tx_float_in_24k[MEM_24], DV_PACKET_SAMPLES );

                                for ( i = 0 ; i < DV_PACKET_SAMPLES ; i++ ) {
                                    cbWriteShort( TX2_cb, ( short )( tx_float_out_8k[i]*SCALE_TX_IN ) );
                                }

                            }

//
//						// Check for >= 320 samples in TX2_cb and spin vocoder
                            // 	Move output to TX3_cb.

                            uint32 decode_out  = 0;

                            if ( csbContains( TX2_cb ) >= DV_PACKET_SAMPLES ) {
                                for ( i = 0 ; i < DV_PACKET_SAMPLES ; i++ ) {
                                    speech_in[i] = cbReadShort( TX2_cb );
                                }

                                /* DECODE */
                                decode_out = aether_vocoder_encode( _dv_serial_handle, speech_in, mod_out, DV_PACKET_SAMPLES );
                            }

                            if ( initial_tx ) {

                                initial_tx = FALSE;

                                _dstar->tx_state = BIT_FRAME_SYNC;

                                dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, NULL);

                                _dstar->tx_state = HEADER_PROCESSING;

                                dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, NULL);

                                slow_data_createEncodeBytes(_dstar);

                                initial_tx_flush = TRUE;

                                dstar_tx_frame_count = 0;
                            } else {
                                /* Data and Voice */

                                if ( decode_out != 0 ) {
                                    _dstar->tx_state = VOICE_FRAME;
                                    dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, mod_out);

                                    if ( dstar_tx_frame_count % 21 == 0 ) {
                                        _dstar->tx_state = DATA_SYNC_FRAME;
                                        dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, NULL);
                                        if ( _dstar->slow_encoder->encode_state == HEADER_TX )
                                            _dstar->slow_encoder->header_index = 0;
                                    } else {

                                        _dstar->tx_state = DATA_FRAME;
                                        dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, NULL);
                                    }

                                    dstar_tx_frame_count++;
                                }
                            }

                            if ( _end_of_transmission && !inhibit_tx ) {

                                _dstar->tx_state = END_PATTERN;

                                dstar_txStateMachine(_dstar, _gmsk_mod, TX4_cb, NULL);

                                flush_tx = TRUE;
                                initial_tx_flush = TRUE;
                            }

                        }

                        if ( !inhibit_tx ) {
                            uint32 tx_check_samples = PACKET_SAMPLES;

                            if ( cfbContains( TX4_cb ) >= tx_check_samples ) {
                                for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                    // Set up the outbound packet
                                    fsample = cbReadFloat( TX4_cb );

                                    // put the fsample into the outbound packet
                                    ( ( Complex * )buf_desc->buf_ptr )[i].real = fsample;
                                    ( ( Complex * )buf_desc->buf_ptr )[i].imag = fsample;
                                }
                            } else {
                                memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof( Complex ) );
                            }

                            emit_waveform_output( buf_desc );

                            if ( flush_tx && initial_tx_flush ) {
                                initial_tx_flush = FALSE;
                                inhibit_tx = TRUE;

                                //output("TX4_cb has %d samples\n", cfbContains(TX4_cb));
                                while ( cfbContains( TX4_cb ) > 0 ) {

                                    if ( cfbContains( TX4_cb ) > PACKET_SAMPLES ) {
                                        for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                            // Set up the outbound packet
                                            fsample = cbReadFloat( TX4_cb );

                                            // put the fsample into the outbound packet
                                            ( ( Complex * )buf_desc->buf_ptr )[i].real = fsample;
                                            ( ( Complex * )buf_desc->buf_ptr )[i].imag = fsample;
                                        }
                                    } else {
                                        int end_index = 0;

                                        for ( i = 0 ; i <= cfbContains( TX4_cb ); i++ ) {
                                            fsample = cbReadFloat( TX4_cb );
                                            ( ( Complex * )buf_desc->buf_ptr )[i].real = fsample;
                                            ( ( Complex * )buf_desc->buf_ptr )[i].imag = fsample;
                                            end_index = i + 1;
                                        }

                                        for ( i = end_index ; i < PACKET_SAMPLES ; i++ ) {
                                            ( ( Complex * )buf_desc->buf_ptr )[i].real = 0.0f;
                                            ( ( Complex * )buf_desc->buf_ptr )[i].imag = 0.0f;
                                        }

                                    }

                                    emit_waveform_output( buf_desc );

                                }
                            }

                        }
                    }

                    hal_BufferRelease( &buf_desc );
                }
            } while ( 1 ); // Seems infinite loop but will exit once there are no longer any buffers linked in _Waveformlist
        }
    }

    _waveform_thread_abort = TRUE;

    gmsk_destroyDemodulator( _gmsk_demod );
    gmsk_destroyModulator( _gmsk_mod );
    dstar_destroyMachine( _dstar );

    return NULL;
}

void sched_waveform_Init( void ) {

    _dstar = dstar_createMachine();

    _gmsk_demod = gmsk_createDemodulator();
    _gmsk_mod = gmsk_createModulator();
    _gmsk_mod->m_invert = TRUE;

    pthread_rwlock_init( &_list_lock, NULL );

    pthread_rwlock_wrlock( &_list_lock );
    _root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _root, 0, sizeof( buffer_descriptor ) );
    _root->next = _root;
    _root->prev = _root;
    pthread_rwlock_unlock( &_list_lock );

    sem_init( &sched_waveform_sem, 0, 0 );

    pthread_create( &_waveform_thread, NULL, &_sched_waveform_thread, NULL );

    struct sched_param fifo_param;
    fifo_param.sched_priority = 30;
    pthread_setschedparam( _waveform_thread, SCHED_FIFO, &fifo_param );
}

void sched_waveformThreadExit() {
    _waveform_thread_abort = TRUE;
    sem_post( &sched_waveform_sem );
}
