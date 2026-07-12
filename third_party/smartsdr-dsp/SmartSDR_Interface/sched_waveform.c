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
#include <errno.h>
#include <time.h>
#include <sys/prctl.h>

#include "common.h"
#include "aether_buffer_queue.h"
#include "aether_dstar_protocol.h"
#include "aether_smartsdr_command.h"
#include "datatypes.h"
#include "hal_buffer.h"
#include "sched_waveform.h"
#include "dstar_transmit_state.h"
#include "dstar_tx_output.h"
#include "dstar_tx_stream.h"
#include "dstar_waveform_metrics.h"
#include "digital_voice_mode_registry.h"
#include "vita.h"
#include "vita_output.h"
#include "aether_vocoder_backend.h"
#include "traffic_cop.h"
#include "ftd2xx.h"

static aether_buffer_queue _waveform_queue;

static pthread_t _waveform_thread;
static BOOL _waveform_thread_abort = FALSE;

static sem_t sched_waveform_sem;

enum {
    DSTAR_MAX_DECODED_AUDIO_BLOCKS_PER_PACKET = 2U,
    DSTAR_CAPTURE_BUFFER_SIZE = 1024U * 1024U,
    DSTAR_RX_TIMING_REPORT_PACKETS = 188U,
    DSTAR_RX_PACKET_BUDGET_US = 5334U,
    DSTAR_RX_SYNC_INTERVAL_SAMPLES = 21U * 96U
        * AETHER_DSTAR_SAMPLES_PER_BIT,
    DSTAR_RX_PLAYOUT_PREBUFFER_SAMPLES =
        160U * ( AETHER_DSTAR_SAMPLE_RATE / 8000U ) * 2U,
    DSTAR_TX_DRAIN_AUDIO_BUDGET_US = 280000U,
    DSTAR_TX_MAX_RESPONSES_PER_TICK = 64U,
    DSTAR_TX_INPUT_FRAME_SAMPLES = 480U,
    DSTAR_WAVEFORM_QUEUE_LIMIT = 128U,
    DSTAR_RX_SYNC_MAX_ERRORS_DEFAULT = 2U,
    /*
     * A zero miss limit leaves the receiver in VOICE forever when the RF end
     * pattern is lost. Three missed 21-frame sync intervals tolerates a short
     * fade while guaranteeing that noise-derived AMBE stops and the next
     * transmission can reacquire from SEARCH.
     */
    DSTAR_RX_SYNC_MISS_LIMIT_DEFAULT = 3U,
    DSTAR_RX_SYNC_REALIGN_BITS_DEFAULT = 24U
};

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
    return aether_buffer_queue_pop( &_waveform_queue );
}

static BOOL _WaveformList_LinkTail( BufferDescriptor buf_desc ) {
    BOOL dropped = FALSE;
    const BOOL was_empty = aether_buffer_queue_push(
        &_waveform_queue, buf_desc, &dropped );
    if ( dropped ) {
        const uint64 drop_count = aether_buffer_queue_dropped(
            &_waveform_queue );
        if ( drop_count == 1U || drop_count % 100U == 0U ) {
            output( "AETHER_DV_DIAG waveform_queue_drop_oldest count=%llu limit=%u\n",
                    ( unsigned long long )drop_count,
                    DSTAR_WAVEFORM_QUEUE_LIMIT );
        }
    }
    return was_empty;
}

static uint32 _WaveformList_Depth( void ) {
    return aether_buffer_queue_depth( &_waveform_queue );
}

void sched_waveform_Schedule( BufferDescriptor buf_desc ) {
    if ( _WaveformList_LinkTail( buf_desc ) ) {
        sem_post( &sched_waveform_sem );
    }
}

void sched_waveform_signal() {
    sem_post( &sched_waveform_sem );
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

static FT_HANDLE _dv_serial_connection_handle = 0;
static FT_HANDLE _dv_serial_handle = 0;
static pthread_rwlock_t _dv_serial_handle_lock;
static pthread_rwlock_t _dstar_config_lock;

static GMSK_DEMOD _gmsk_demod = NULL;
static GMSK_MOD   _gmsk_mod = NULL;
static aether_dstar_protocol _dstar_storage;
static aether_dstar_protocol * _dstar = &_dstar_storage;
static aether_dstar_tx_snapshot _dstar_tx_snapshot;
static BOOL _dstar_tx_snapshot_valid = FALSE;
static BOOL _dstar_rx_status_active = FALSE;
static char _dstar_last_rx_status[512];
/* Suppress re-emitting an identical transmit status (mirrors the RX path).
 * `waveform ... status` requests re-run _dstar_send_tx_status at will, so
 * without this a client status poll during an active transmission would make
 * the client log a duplicate TX entry. Reset on each transmit begin/cancel so
 * a genuine new transmission always emits its first status. */
static BOOL _dstar_tx_status_active = FALSE;
static char _dstar_last_tx_status[512];
static BOOL _dstar_rx_message_status_active = FALSE;
static char _dstar_last_rx_message_status[256];
static dstar_transmit_state _transmit_state = DSTAR_TRANSMIT_STATE_INITIALIZER;
static dstar_waveform_metric_state _rx_waveform_metrics;
static dstar_waveform_metric_state _tx_waveform_metrics;
static float _tx_sample_gain = 1.0f;
static BOOL _verbose_rx_idle_diag = FALSE;
static BOOL _diag_rx_timing = FALSE;
static BOOL _diag_rx_turnaround = FALSE;
static BOOL _diag_sync_timing = FALSE;
static BOOL _force_null_tx_ambe = FALSE;
static FILE * _rx_sample_capture = NULL;
static FILE * _tx_sample_capture = NULL;
static FILE * _rx_metadata_capture = NULL;
static uint64 _rx_sample_capture_count = 0U;
static uint64 _tx_sample_capture_count = 0U;
static uint64 _rx_input_sample_offset = 0U;
static uint64 _rx_metadata_packet_count = 0U;

#define FREEDV_NSAMPLES 160

static BOOL _dstar_env_bool( const char * name, BOOL default_value ) {
    const char * value = getenv( name );
    if ( value == NULL || value[0] == '\0' ) {
        return default_value;
    }

    const int first = tolower( ( unsigned char )value[0] );
    if ( first == '0' || first == 'f' || first == 'n' || first == 'o' ) {
        return FALSE;
    }
    if ( first == '1' || first == 't' || first == 'y' ) {
        return TRUE;
    }

    return default_value;
}

static float _dstar_env_float( const char * name,
                               float default_value,
                               float min_value,
                               float max_value ) {
    const char * value = getenv( name );
    if ( value == NULL || value[0] == '\0' ) {
        return default_value;
    }

    char * end = NULL;
    float parsed = strtof( value, &end );
    if ( end == value || *end != '\0' || !dstar_tx_output_isFinite( parsed ) ) {
        return default_value;
    }

    if ( parsed < min_value ) {
        return min_value;
    }
    if ( parsed > max_value ) {
        return max_value;
    }
    return parsed;
}

static uint32 _dstar_env_uint( const char * name,
                               uint32 default_value,
                               uint32 min_value,
                               uint32 max_value ) {
    const char * value = getenv( name );
    if ( value == NULL || value[0] == '\0' ) {
        return default_value;
    }

    uint32 parsed = 0U;
    const char * end = NULL;
    if ( aether_smartsdr_parse_uint32(
             value, 10, &parsed, &end ) != 0
         || *end != '\0' ) {
        return default_value;
    }
    if ( parsed < min_value ) {
        return min_value;
    }
    if ( parsed > max_value ) {
        return max_value;
    }
    return parsed;
}

static BOOL _dstar_transmit_or_tail_enabled( void ) {
    return dstar_transmit_state_transmitOrTailEnabled( &_transmit_state );
}

static BOOL _dstar_consume_receive_reset_request( void ) {
    return dstar_transmit_state_consumeReceiveReset( &_transmit_state );
}

static void _dstar_finish_transmit_tail( void ) {
    dstar_transmit_state_finishTail( &_transmit_state );
    sched_waveform_signal();
}

static float _dstar_abs_float( float value ) {
    return value < 0.0f ? -value : value;
}

static uint64 _dstar_elapsed_usec( const struct timespec * start,
                                  const struct timespec * end ) {
    const int64 seconds = ( int64 )end->tv_sec - ( int64 )start->tv_sec;
    const int64 nanoseconds = ( int64 )end->tv_nsec - ( int64 )start->tv_nsec;
    const int64 elapsed_nanoseconds = seconds * 1000000000LL + nanoseconds;
    return elapsed_nanoseconds > 0 ? ( uint64 )elapsed_nanoseconds / 1000U : 0U;
}

static uint64 _dstar_monotonic_ns( void ) {
    struct timespec now;
    clock_gettime( CLOCK_MONOTONIC, &now );
    return ( uint64 )now.tv_sec * 1000000000ULL + ( uint64 )now.tv_nsec;
}

static void _dstar_set_deadline_after_usec( struct timespec * deadline,
                                            uint32 usec ) {
    if ( deadline == NULL ) {
        return;
    }
    clock_gettime( CLOCK_REALTIME, deadline );
    deadline->tv_sec += ( time_t )( usec / 1000000U );
    deadline->tv_nsec += ( long )( usec % 1000000U ) * 1000L;
    if ( deadline->tv_nsec >= 1000000000L ) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static uint32 _dstar_collect_tx_ambe( dstar_tx_stream * stream ) {
    if ( stream == NULL || stream->phase == DSTAR_TX_STREAM_IDLE ) {
        return 0U;
    }

    uint32 collected = 0U;
    while ( collected < DSTAR_TX_MAX_RESPONSES_PER_TICK ) {
        unsigned char ambe[CRDV_AMBE_BYTES] = {0};
        uint64_t sequence = 0U;
        const uint32 encoded_bytes = aether_vocoder_take_encoded(
            ambe, sizeof( ambe ), &sequence );
        if ( encoded_bytes == 0U ) {
            break;
        }
        dstar_tx_stream_offerVoice(
            stream, _gmsk_mod, TX4_cb,
            ( uint64 )sequence, ambe,
            encoded_bytes == CRDV_AMBE_BYTES );
        collected++;
    }
    return collected;
}

static BOOL _dstar_prepare_tx_speech( float * input_24k,
                                      float * output_8k,
                                      short * speech,
                                      BOOL pad_partial,
                                      uint32 * clip_count,
                                      uint32 * invalid_count ) {
    if ( input_24k == NULL || output_8k == NULL || speech == NULL
         || clip_count == NULL || invalid_count == NULL ) {
        return FALSE;
    }

    uint32 available = ( uint32 )cfbContains( TX1_cb );
    if ( available < DSTAR_TX_INPUT_FRAME_SAMPLES && !pad_partial ) {
        return FALSE;
    }
    if ( available == 0U ) {
        return FALSE;
    }
    if ( available > DSTAR_TX_INPUT_FRAME_SAMPLES ) {
        available = DSTAR_TX_INPUT_FRAME_SAMPLES;
    }

    for ( uint32 i = 0U; i < DSTAR_TX_INPUT_FRAME_SAMPLES; i++ ) {
        input_24k[i + MEM_24] = i < available
            ? cbReadFloat( TX1_cb )
            : 0.0f;
    }
    fdmdv_24_to_8(
        output_8k, &input_24k[MEM_24], DV_PACKET_SAMPLES );
    for ( uint32 i = 0U; i < DV_PACKET_SAMPLES; i++ ) {
        BOOL clipped = FALSE;
        BOOL invalid = FALSE;
        speech[i] = dstar_tx_output_pcm16(
            output_8k[i], &clipped, &invalid );
        if ( clipped ) {
            ( *clip_count )++;
        }
        if ( invalid ) {
            ( *invalid_count )++;
        }
    }
    return TRUE;
}

static BOOL _dstar_submit_tx_speech( dstar_tx_stream * stream,
                                     const short * speech,
                                     uint64 sequence ) {
    if ( stream == NULL || speech == NULL ) {
        return FALSE;
    }
    if ( _force_null_tx_ambe ) {
        dstar_tx_stream_offerVoice(
            stream, _gmsk_mod, TX4_cb,
            sequence, NULL, FALSE );
        return TRUE;
    }
    BOOL submitted = FALSE;
    pthread_rwlock_rdlock( &_dv_serial_handle_lock );
    if ( _dv_serial_handle != NULL ) {
        submitted = aether_vocoder_submit_encode(
            _dv_serial_handle, speech, DV_PACKET_SAMPLES, sequence );
    }
    pthread_rwlock_unlock( &_dv_serial_handle_lock );
    return submitted;
}

static void _dstar_record_rx_metrics( BufferDescriptor buf_desc ) {
    if ( buf_desc == NULL || buf_desc->arrival_monotonic_ns == 0U ) {
        return;
    }

    const uint64 now_ns = _dstar_monotonic_ns();
    const uint64 turnaround_us = now_ns > buf_desc->arrival_monotonic_ns
        ? ( now_ns - buf_desc->arrival_monotonic_ns ) / 1000U
        : 0U;
    const BOOL timestamp_valid =
        ( buf_desc->vita_header & VITA_HEADER_TSI_MASK ) == VITA_TSI_UTC
        && ( buf_desc->vita_header & VITA_HEADER_TSF_MASK ) == VITA_TSF_REAL_TIME;
    const uint64 timestamp_frac =
        ( ( uint64 )buf_desc->timestamp_frac_h << 32U )
        | ( uint64 )buf_desc->timestamp_frac_l;
    dstar_waveform_metric_snapshot snapshot;
    if ( dstar_waveform_metrics_record_packet(
             &_rx_waveform_metrics,
             buf_desc->stream_id,
             timestamp_valid,
             buf_desc->timestamp_int,
             timestamp_frac,
             buf_desc->num_samples,
             buf_desc->vita_missing_packets,
             turnaround_us,
             DSTAR_RX_PACKET_BUDGET_US,
             _WaveformList_Depth(),
             &snapshot ) ) {
        output( "AETHER_DV_METRIC v=2 mode=%s dir=RX rate_hz=%.1f vita_gaps=%u source_blocks=%u turn_mean_us=%.1f turn_max_us=%llu queue_max=%u\n",
                digital_voice_mode_registry_mode(),
                snapshot.rx_sample_rate_hz,
                snapshot.vita_sequence_gaps,
                snapshot.source_block_deficits,
                snapshot.turnaround_mean_us,
                ( unsigned long long )snapshot.turnaround_max_us,
                snapshot.queue_max );
        if ( _diag_rx_turnaround ) {
            output( "AETHER_DSTAR_DIAG rx_turnaround packets=%u mean_us=%.1f max_us=%llu over_budget=%u\n",
                    snapshot.interval_count,
                    snapshot.turnaround_mean_us,
                    ( unsigned long long )snapshot.turnaround_max_us,
                    snapshot.turnaround_over_budget );
        }
    }
}

static void _dstar_output_tx_metrics(
    const dstar_waveform_metric_snapshot * snapshot ) {
    if ( snapshot == NULL ) {
        return;
    }
    output( "AETHER_DV_METRIC v=3 mode=%s dir=TX rate_hz=%.1f vita_gaps=%u null_frames=%u pcm_clips=%u pcm_invalid=%u send_failures=%u queue_max=%u tail_samples=%u tail_us=%llu preroll_frames=%u preroll_delay_ms=%u ambe_queue_max=%u ambe_underflows=%u ambe_overflows=%u ambe_sequence_errors=%u vocoder_submit_failures=%u vocoder_pending_max=%u drain_frames=%u drain_timeouts=%u drain_discarded_frames=%u\n",
            digital_voice_mode_registry_mode(),
            snapshot->rx_sample_rate_hz,
            snapshot->vita_sequence_gaps,
            snapshot->null_frames,
            snapshot->pcm_clips,
            snapshot->pcm_invalid,
            snapshot->send_failures,
            snapshot->queue_max,
            snapshot->tail_samples,
            ( unsigned long long )snapshot->tail_us,
            snapshot->pre_roll_frames,
            snapshot->pre_roll_delay_ms,
            snapshot->ambe_queue_max,
            snapshot->ambe_underflows,
            snapshot->ambe_overflows,
            snapshot->ambe_sequence_errors,
            snapshot->vocoder_submit_failures,
            snapshot->vocoder_pending_max,
            snapshot->drain_frames,
            snapshot->drain_timeouts,
            snapshot->drain_discarded_frames );
}

static void _dstar_record_tx_packet_metrics( BufferDescriptor buf_desc,
                                             uint32 queue_depth ) {
    if ( buf_desc == NULL || buf_desc->arrival_monotonic_ns == 0U ) {
        return;
    }
    const BOOL timestamp_valid =
        ( buf_desc->vita_header & VITA_HEADER_TSI_MASK ) == VITA_TSI_UTC
        && ( buf_desc->vita_header & VITA_HEADER_TSF_MASK )
            == VITA_TSF_REAL_TIME;
    const uint64 timestamp_frac =
        ( ( uint64 )buf_desc->timestamp_frac_h << 32U )
        | ( uint64 )buf_desc->timestamp_frac_l;
    dstar_waveform_metric_snapshot snapshot;
    if ( dstar_waveform_metrics_record_packet(
             &_tx_waveform_metrics,
             buf_desc->stream_id,
             timestamp_valid,
             buf_desc->timestamp_int,
             timestamp_frac,
             buf_desc->num_samples,
             buf_desc->vita_missing_packets,
             0U,
             DSTAR_RX_PACKET_BUDGET_US,
             queue_depth,
             &snapshot ) ) {
        _dstar_output_tx_metrics( &snapshot );
    }
}

static float _dstar_scale_tx_sample( float sample ) {
    return dstar_tx_output_scaleSample( sample, _tx_sample_gain );
}

static FILE * _dstar_open_sample_capture( const char * environment_name ) {
    const char * path = getenv( environment_name );
    if ( path == NULL || path[0] == '\0' ) {
        return NULL;
    }

    FILE * capture = fopen( path, "wb" );
    if ( capture == NULL ) {
        output( "AETHER_DSTAR_DIAG sample_capture_open_failed env=%s path=%s errno=%d\n",
                environment_name,
                path,
                errno );
        return NULL;
    }

    setvbuf( capture, NULL, _IOFBF, DSTAR_CAPTURE_BUFFER_SIZE );

    output( "AETHER_DSTAR_DIAG sample_capture_open env=%s path=%s format=f32le rate=%u\n",
            environment_name,
            path,
            AETHER_DSTAR_SAMPLE_RATE );
    return capture;
}

static FILE * _dstar_open_metadata_capture( const char * environment_name ) {
    const char * path = getenv( environment_name );
    if ( path == NULL || path[0] == '\0' ) {
        return NULL;
    }

    FILE * capture = fopen( path, "w" );
    if ( capture == NULL ) {
        output( "AETHER_DSTAR_DIAG metadata_capture_open_failed env=%s path=%s errno=%d\n",
                environment_name,
                path,
                errno );
        return NULL;
    }

    setvbuf( capture, NULL, _IOFBF, DSTAR_CAPTURE_BUFFER_SIZE );
    fprintf( capture,
             "packet_index,sample_offset,sample_count,stream_id,vita_packet_count,"
             "vita_header,timestamp_int,timestamp_frac,arrival_monotonic_ns\n" );
    output( "AETHER_DSTAR_DIAG metadata_capture_open env=%s path=%s format=csv\n",
            environment_name,
            path );
    return capture;
}

static void _dstar_capture_real_samples( FILE ** capture_ptr,
                                         uint64 * sample_count,
                                         const Complex * samples,
                                         uint32 length ) {
    if ( capture_ptr == NULL || *capture_ptr == NULL || sample_count == NULL
         || samples == NULL || length == 0U ) {
        return;
    }

    FILE * capture = *capture_ptr;
    float real_samples[PACKET_SAMPLES];
    uint32 offset = 0U;
    while ( offset < length ) {
        uint32 block_length = length - offset;
        if ( block_length > PACKET_SAMPLES ) {
            block_length = PACKET_SAMPLES;
        }

        for ( uint32 i = 0U; i < block_length; i++ ) {
            real_samples[i] = samples[offset + i].real;
        }

        const size_t written = fwrite( real_samples, sizeof( float ), block_length, capture );
        *sample_count += written;
        if ( written != block_length ) {
            output( "AETHER_DSTAR_DIAG sample_capture_write_failed expected=%u written=%zu errno=%d\n",
                    block_length,
                    written,
                    errno );
            fclose( capture );
            *capture_ptr = NULL;
            return;
        }
        offset += block_length;
    }

}

static void _dstar_capture_rx_packet( BufferDescriptor buf_desc,
                                      const Complex * samples ) {
    if ( buf_desc == NULL || samples == NULL ) {
        return;
    }

    const uint64 sample_offset = _rx_input_sample_offset;
    _dstar_capture_real_samples(
        &_rx_sample_capture,
        &_rx_sample_capture_count,
        samples,
        buf_desc->num_samples );

    if ( _rx_metadata_capture != NULL ) {
        const uint64 timestamp_frac =
            ( ( uint64 )buf_desc->timestamp_frac_h << 32U )
            | ( uint64 )buf_desc->timestamp_frac_l;
        const int written = fprintf(
            _rx_metadata_capture,
            "%llu,%llu,%u,0x%08X,%u,0x%08X,%u,%llu,%llu\n",
            ( unsigned long long )_rx_metadata_packet_count,
            ( unsigned long long )sample_offset,
            buf_desc->num_samples,
            buf_desc->stream_id,
            buf_desc->vita_packet_count,
            buf_desc->vita_header,
            buf_desc->timestamp_int,
            ( unsigned long long )timestamp_frac,
            ( unsigned long long )buf_desc->arrival_monotonic_ns );
        if ( written < 0 ) {
            output( "AETHER_DSTAR_DIAG metadata_capture_write_failed errno=%d\n", errno );
            fclose( _rx_metadata_capture );
            _rx_metadata_capture = NULL;
        }
    }

    _rx_input_sample_offset += buf_desc->num_samples;
    _rx_metadata_packet_count++;
}

static BOOL _dstar_header_status( char * destination,
                                  size_t capacity,
                                  uint32 slice,
                                  const crdv_header_fields * fields,
                                  const char * direction ) {
    char rpt2[9];
    char rpt1[9];
    char urcall[9];
    char mycall[9];
    char suffix[5];
    if ( destination == NULL || capacity == 0U || fields == NULL
         || direction == NULL ) {
        return FALSE;
    }

    aether_smartsdr_encode_status_value(
        rpt2, sizeof( rpt2 ), fields->rpt2, sizeof( fields->rpt2 ) );
    aether_smartsdr_encode_status_value(
        rpt1, sizeof( rpt1 ), fields->rpt1, sizeof( fields->rpt1 ) );
    aether_smartsdr_encode_status_value(
        urcall, sizeof( urcall ), fields->urcall, sizeof( fields->urcall ) );
    aether_smartsdr_encode_status_value(
        mycall, sizeof( mycall ), fields->mycall, sizeof( fields->mycall ) );
    aether_smartsdr_encode_status_value(
        suffix, sizeof( suffix ), fields->suffix, sizeof( fields->suffix ) );

    const int written = snprintf(
        destination,
        capacity,
        "waveform status slice=%u destination_rptr_%s=%s departure_rptr_%s=%s companion_call_%s=%s own_call1_%s=%s own_call2_%s=%s",
        slice,
        direction,
        rpt2,
        direction,
        rpt1,
        direction,
        urcall,
        direction,
        mycall,
        direction,
        suffix );
    return written > 0 && ( size_t )written < capacity;
}

static uint32 _dstar_slice( void ) {
    pthread_rwlock_rdlock( &_dstar_config_lock );
    const uint32 slice = _dstar->slice;
    pthread_rwlock_unlock( &_dstar_config_lock );
    return slice;
}

static BOOL _dstar_verbose( void ) {
    pthread_rwlock_rdlock( &_dstar_config_lock );
    const BOOL verbose = _dstar->verbose;
    pthread_rwlock_unlock( &_dstar_config_lock );
    return verbose;
}

static BOOL _dstar_copy_tx_snapshot( aether_dstar_tx_snapshot * snapshot ) {
    pthread_rwlock_rdlock( &_dstar_config_lock );
    const BOOL valid = _dstar_tx_snapshot_valid;
    if ( valid && snapshot != NULL ) {
        *snapshot = _dstar_tx_snapshot;
    }
    pthread_rwlock_unlock( &_dstar_config_lock );
    return valid && snapshot != NULL;
}

static void _dstar_send_tx_status( uint32 slice,
                                   const aether_dstar_tx_snapshot * snapshot ) {
    char status[512];
    char message[CRDV_MESSAGE_BYTES + 1U];
    if ( snapshot == NULL
         || !_dstar_header_status(
                status, sizeof( status ), slice, &snapshot->fields, "tx" ) ) {
        return;
    }

    aether_smartsdr_encode_status_value(
        message,
        sizeof( message ),
        ( const char * )snapshot->message,
        sizeof( snapshot->message ) );
    const size_t used = strlen( status );
    const int written = snprintf(
        status + used, sizeof( status ) - used, " message_tx=%s", message );
    if ( written < 0 || ( size_t )written >= sizeof( status ) - used ) {
        return;
    }
    if ( _dstar_tx_status_active
         && strcmp( _dstar_last_tx_status, status ) == 0 ) {
        return;
    }
    tc_sendSmartSDRcommand( status, FALSE, NULL );
    snprintf( _dstar_last_tx_status,
              sizeof( _dstar_last_tx_status ), "%s", status );
    _dstar_tx_status_active = TRUE;
}

static void _dstar_send_rx_header( uint32 slice,
                                   const crdv_header_fields * fields ) {
    char status[512];
    if ( !_dstar_header_status(
             status, sizeof( status ), slice, fields, "rx" ) ) {
        return;
    }
    if ( _dstar_rx_status_active
         && strcmp( _dstar_last_rx_status, status ) == 0 ) {
        return;
    }
    tc_sendSmartSDRcommand( status, FALSE, NULL );
    snprintf( _dstar_last_rx_status,
              sizeof( _dstar_last_rx_status ), "%s", status );
    _dstar_rx_status_active = TRUE;
}

static void _dstar_send_rx_message( uint32 slice, const char * text ) {
    char encoded[CRDV_MESSAGE_BYTES + 1U];
    char status[256];
    if ( text == NULL ) {
        return;
    }
    aether_smartsdr_encode_status_value(
        encoded, sizeof( encoded ), text, CRDV_MESSAGE_BYTES );
    const int written = snprintf(
        status, sizeof( status ),
        "waveform status slice=%u message=%s", slice, encoded );
    if ( written <= 0 || ( size_t )written >= sizeof( status ) ) {
        return;
    }
    if ( _dstar_rx_message_status_active
         && strcmp( _dstar_last_rx_message_status, status ) == 0 ) {
        return;
    }
    tc_sendSmartSDRcommand( status, FALSE, NULL );
    snprintf( _dstar_last_rx_message_status,
              sizeof( _dstar_last_rx_message_status ), "%s", status );
    _dstar_rx_message_status_active = TRUE;
}

static void _dstar_send_rx_end( uint32 slice ) {
    char status[64];
    const int written = snprintf(
        status, sizeof( status ), "waveform status slice=%u RX=END", slice );
    if ( written > 0 && ( size_t )written < sizeof( status ) ) {
        tc_sendSmartSDRcommand( status, FALSE, NULL );
    }
    _dstar_rx_status_active = FALSE;
    _dstar_last_rx_status[0] = '\0';
    _dstar_rx_message_status_active = FALSE;
    _dstar_last_rx_message_status[0] = '\0';
}

void sched_waveform_sendStatus( uint32 slice ) {
    aether_dstar_tx_snapshot snapshot;
    pthread_rwlock_rdlock( &_dstar_config_lock );
    const crdv_result result = aether_dstar_protocol_tx_snapshot(
        _dstar, &snapshot );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( result == CRDV_OK ) {
        _dstar_send_tx_status( slice, &snapshot );
    }
}

static void _dstar_report_invalid_setting( const char * name ) {
    output( "AETHER_DSTAR_ERROR invalid %s configuration value\n", name );
}

void sched_waveform_setDestinationRptr( uint32 slice,
                                        const char * destination_rptr ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_rpt2(
        _dstar, destination_rptr );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "RPT2" );
    }
}

void sched_waveform_setDepartureRptr( uint32 slice,
                                      const char * departure_rptr ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_rpt1(
        _dstar, departure_rptr );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "RPT1" );
    }
}

void sched_waveform_setCompanionCall( uint32 slice,
                                      const char * companion_call ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_urcall(
        _dstar, companion_call );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "URCALL" );
    }
}

void sched_waveform_setOwnCall1( uint32 slice, const char * owncall1 ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_mycall(
        _dstar, owncall1 );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "MYCALL" );
    }
}

void sched_waveform_setOwnCall2( uint32 slice, const char * owncall2 ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_suffix(
        _dstar, owncall2 );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "MYCALL suffix" );
    }
}

void sched_waveform_setMessage( uint32 slice, const char * message ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    const BOOL accepted = aether_dstar_protocol_set_message(
        _dstar, message );
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( !accepted ) {
        _dstar_report_invalid_setting( "message" );
    }
}

BOOL sched_waveform_configureDStar( uint32 slice,
                                    const char * mycall,
                                    const char * suffix,
                                    const char * urcall,
                                    const char * rpt1,
                                    const char * rpt2,
                                    const char * message ) {
    pthread_rwlock_wrlock( &_dstar_config_lock );
    const crdv_result result = aether_dstar_protocol_configure(
        _dstar, mycall, suffix, urcall, rpt1, rpt2, message );
    if ( result == CRDV_OK ) {
        aether_dstar_protocol_set_slice( _dstar, slice );
    }
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( result != CRDV_OK ) {
        output( "AETHER_DSTAR_ERROR station configuration rejected result=%d\n",
                result );
    }
    return result == CRDV_OK;
}

void sched_waveform_setHandle( FT_HANDLE * handle ) {
    pthread_rwlock_wrlock( &_dv_serial_handle_lock );
    _dv_serial_handle = *handle;
    pthread_rwlock_unlock( &_dv_serial_handle_lock );
}

void sched_waveform_beginTransmit( void ) {
    if ( dstar_transmit_state_phase( &_transmit_state )
         != DSTAR_TRANSMIT_IDLE ) {
        return;
    }

    aether_dstar_tx_snapshot snapshot;
    uint32 slice = 0U;
    pthread_rwlock_wrlock( &_dstar_config_lock );
    const crdv_result result = aether_dstar_protocol_tx_snapshot(
        _dstar, &snapshot );
    if ( result == CRDV_OK ) {
        _dstar_tx_snapshot = snapshot;
        _dstar_tx_snapshot_valid = TRUE;
        slice = _dstar->slice;
    }
    pthread_rwlock_unlock( &_dstar_config_lock );
    if ( result != CRDV_OK ) {
        output( "AETHER_DSTAR_ERROR transmit configuration rejected result=%d\n",
                result );
        return;
    }

    if ( dstar_transmit_state_begin( &_transmit_state ) ) {
        /* New transmission — clear suppression so its first status always
         * emits even if identical to the previous transmission's. */
        _dstar_tx_status_active = FALSE;
        _dstar_send_tx_status( slice, &snapshot );
        sched_waveform_signal();
    }
}

void sched_waveform_requestTransmitEnd( void ) {
    if ( dstar_transmit_state_requestEnd( &_transmit_state ) ) {
        sched_waveform_signal();
    }
}

void sched_waveform_cancelTransmit( void ) {
    if ( dstar_transmit_state_cancel( &_transmit_state ) ) {
        pthread_rwlock_wrlock( &_dstar_config_lock );
        _dstar_tx_snapshot_valid = FALSE;
        pthread_rwlock_unlock( &_dstar_config_lock );
        _dstar_tx_status_active = FALSE;
        _dstar_last_tx_status[0] = '\0';
        sched_waveform_signal();
    }
}

void sched_waveform_requestReceiveReset( void ) {
    dstar_transmit_state_requestReceiveReset( &_transmit_state );
    sched_waveform_signal();
}

void sched_waveform_setDSTARSlice( uint32 slice )
{
    pthread_rwlock_wrlock( &_dstar_config_lock );
    aether_dstar_protocol_set_slice( _dstar, slice );
    pthread_rwlock_unlock( &_dstar_config_lock );
}

static void _dstar_reset_rx_machine( void ) {
    if ( _gmsk_demod != NULL ) {
        gmskDemod_reset( _gmsk_demod );
    }
    aether_dstar_protocol_reset_rx( _dstar );
    _dstar_rx_status_active = FALSE;
    _dstar_last_rx_status[0] = '\0';
    _dstar_rx_message_status_active = FALSE;
    _dstar_last_rx_message_status[0] = '\0';
}

static void * _sched_waveform_thread( void * param ) {

    prctl(PR_SET_NAME, "DV-SchedWav");
    int 	nout;

    int		i;			// for loop counter
    float	fsample;	// a float sample
//    float   Sig2Noise;	// Signal to noise ratio

    // Flags ...
    int		initial_rx = TRUE;			// Flags for RX circular buffer, clear if starting receive

    // VOCODER I/O BUFFERS
    short	speech_in[DV_PACKET_SAMPLES];
    short 	speech_out[DV_PACKET_SAMPLES];
    //short 	demod_in[FREEDV_NSAMPLES];
    //unsigned char packet_out[FREEDV_NSAMPLES];

    // RX RESAMPLER I/O BUFFERS
    float 	float_in_8k[DV_PACKET_SAMPLES + FILTER_TAPS];
    //float 	float_out_8k[DV_PACKET_SAMPLES];

    float 	float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];
    float 	float_out_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR ];

    // TX RESAMPLER I/O BUFFERS
    float 	tx_float_out_8k[DV_PACKET_SAMPLES];

    float 	tx_float_in_24k[DV_PACKET_SAMPLES * DECIMATION_FACTOR + FILTER_TAPS];

    // =======================  Initialization Section =========================

    aether_vocoder_init( &_dv_serial_connection_handle );

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

    initial_rx = TRUE;
    dstar_tx_stream tx_stream = DSTAR_TX_STREAM_INITIALIZER;
    struct timespec dstar_tx_tail_deadline = {0};
    BOOL dstar_tx_tail_deadline_valid = FALSE;
    uint32 dstar_rx_input_buffer_count = 0;
    uint32 dstar_rx_ambe_frame_count = 0;
    uint32 dstar_rx_data_sync_count = 0;
    uint32 dstar_rx_audio_block_count = 0;
    uint32 dstar_rx_output_buffer_count = 0;
    uint32 dstar_rx_zero_output_buffer_count = 0;
    uint32 dstar_tx_input_buffer_count = 0;
    uint32 dstar_tx_output_buffer_count = 0;
    uint32 dstar_tx_zero_output_buffer_count = 0;
    uint32 dstar_tx_voice_frame_count = 0;
    uint32 dstar_tx_null_voice_frame_count = 0;
    uint32 dstar_tx_pcm_clip_count = 0U;
    uint32 dstar_tx_pcm_invalid_count = 0U;
    uint32 dstar_tx_send_failure_count = 0U;
    uint32 dstar_tx_vocoder_submit_failure_count = 0U;
    uint32 dstar_tx_vocoder_pending_max = 0U;
    uint32 dstar_tx_drain_timeout_count = 0U;
    uint32 dstar_tx_drain_initial_frames = 0U;
    uint32 dstar_tx_tail_samples = 0U;
    uint64 dstar_tx_tail_started_ns = 0U;
    uint64 dstar_tx_drain_started_ns = 0U;
    uint64 dstar_tx_next_sequence = 0U;
    BOOL dstar_tx_partial_flushed = FALSE;
    BOOL dstar_tx_drain_budget_trimmed = FALSE;
    uint32 dstar_rx_timing_packet_count = 0U;
    uint32 dstar_rx_timing_over_budget_count = 0U;
    uint32 dstar_rx_timing_queue_max = 0U;
    uint32 dstar_rx_timing_vocoder_calls = 0U;
    uint32 dstar_rx_timing_audio_blocks = 0U;
    uint32 dstar_rx_timing_output_packets = 0U;
    uint32 dstar_rx_timing_zero_packets = 0U;
    uint64 dstar_rx_timing_total_us = 0U;
    uint64 dstar_rx_timing_max_us = 0U;
    uint64 dstar_rx_timing_vocoder_total_us = 0U;
    uint64 dstar_rx_timing_vocoder_max_us = 0U;
    BOOL dstar_rx_playout_started = FALSE;
    uint64 dstar_rx_demod_sample_count = 0U;
    uint64 dstar_rx_last_sync_sample = 0U;

    // show that we are running
    BufferDescriptor buf_desc;

    while ( !_waveform_thread_abort ) {
        BOOL tail_wait_timed_out = FALSE;
        const enum dstar_transmit_phase wait_phase =
            dstar_transmit_state_phase( &_transmit_state );
        if ( ( wait_phase == DSTAR_TRANSMIT_DRAINING
               || wait_phase == DSTAR_TRANSMIT_ENDING )
             && dstar_tx_tail_deadline_valid ) {
            if ( sem_timedwait( &sched_waveform_sem,
                                &dstar_tx_tail_deadline ) != 0
                 && errno == ETIMEDOUT ) {
                tail_wait_timed_out = TRUE;
            }
        } else {
            sem_wait( &sched_waveform_sem );
        }

        if ( !_waveform_thread_abort ) {
            enum dstar_transmit_phase current_phase =
                dstar_transmit_state_phase( &_transmit_state );
            if ( current_phase != DSTAR_TRANSMIT_DRAINING
                 && current_phase != DSTAR_TRANSMIT_ENDING ) {
                dstar_tx_tail_deadline_valid = FALSE;
            }

            if ( current_phase == DSTAR_TRANSMIT_IDLE
                 && tx_stream.phase != DSTAR_TX_STREAM_IDLE ) {
                aether_vocoder_flush_lists();
                dstar_tx_stream_reset( &tx_stream, TX4_cb );
                TX1_cb->start = TX1_cb->end = 0U;
                TX2_cb->start = TX2_cb->end = 0U;
                TX3_cb->start = TX3_cb->end = 0U;
                gmsk_resetMODFilter( _gmsk_mod );
                dstar_tx_drain_started_ns = 0U;
                dstar_tx_partial_flushed = FALSE;
                output( "AETHER_DSTAR_DIAG tx_hard_cancel_flushed\n" );
            }

            if ( current_phase == DSTAR_TRANSMIT_DRAINING
                 && ( tx_stream.phase == DSTAR_TX_STREAM_IDLE
                      || ( tx_stream.phase == DSTAR_TX_STREAM_HEADER
                           && !dstar_tx_stream_preambleComplete(
                                  &tx_stream, TX4_cb ) ) ) ) {
                if ( tx_stream.phase == DSTAR_TX_STREAM_HEADER ) {
                    output( "AETHER_DSTAR_DIAG tx_header_aborted_on_early_unkey emitted_samples=%u preamble_samples=%u queued_frames=%u outstanding_vocoder=%u partial_samples=%u\n",
                            dstar_tx_stream_emittedHeaderSamples(
                                &tx_stream, TX4_cb ),
                            tx_stream.header_preamble_samples,
                            dstar_tx_stream_pendingVoiceFrames( &tx_stream ),
                            aether_vocoder_encode_outstanding(),
                            cfbContains( TX1_cb ) );
                    dstar_tx_stream_abortHeader( &tx_stream, TX4_cb );
                } else {
                    output( "AETHER_DSTAR_DIAG tx_aborted_before_stream_started outstanding_vocoder=%u partial_samples=%u\n",
                            aether_vocoder_encode_outstanding(),
                            cfbContains( TX1_cb ) );
                    dstar_tx_stream_reset( &tx_stream, TX4_cb );
                }
                aether_vocoder_flush_lists();
                TX1_cb->start = TX1_cb->end = 0U;
                TX2_cb->start = TX2_cb->end = 0U;
                TX3_cb->start = TX3_cb->end = 0U;
                gmsk_resetMODFilter( _gmsk_mod );
                dstar_tx_drain_started_ns = 0U;
                dstar_tx_partial_flushed = FALSE;
                dstar_tx_drain_budget_trimmed = FALSE;
                dstar_tx_tail_deadline_valid = FALSE;
                sched_waveform_cancelTransmit();
                current_phase = DSTAR_TRANSMIT_IDLE;
            }

            if ( current_phase == DSTAR_TRANSMIT_DRAINING ) {
                const uint64 now_ns = _dstar_monotonic_ns();
                if ( dstar_tx_drain_started_ns == 0U ) {
                    dstar_tx_drain_started_ns = now_ns;
                    dstar_tx_tail_started_ns = now_ns;
                    output( "AETHER_DSTAR_DIAG tx_drain_started queued_frames=%u pending_vocoder=%u partial_samples=%u\n",
                            dstar_tx_stream_pendingVoiceFrames( &tx_stream ),
                            aether_vocoder_pending_encode_requests(),
                            cfbContains( TX1_cb ) );
                }

                if ( !dstar_tx_partial_flushed ) {
                    while ( _dstar_prepare_tx_speech(
                                tx_float_in_24k,
                                tx_float_out_8k,
                                speech_in,
                                FALSE,
                                &dstar_tx_pcm_clip_count,
                                &dstar_tx_pcm_invalid_count ) ) {
                        const uint64 sequence = dstar_tx_next_sequence++;
                        dstar_tx_voice_frame_count++;
                        if ( !_dstar_submit_tx_speech(
                                 &tx_stream, speech_in, sequence ) ) {
                            dstar_tx_vocoder_submit_failure_count++;
                            dstar_tx_stream_offerVoice(
                                &tx_stream, _gmsk_mod, TX4_cb,
                                sequence, NULL, FALSE );
                        }
                    }
                    if ( _dstar_prepare_tx_speech(
                             tx_float_in_24k,
                             tx_float_out_8k,
                             speech_in,
                             TRUE,
                             &dstar_tx_pcm_clip_count,
                             &dstar_tx_pcm_invalid_count ) ) {
                        const uint64 sequence = dstar_tx_next_sequence++;
                        dstar_tx_voice_frame_count++;
                        if ( !_dstar_submit_tx_speech(
                                 &tx_stream, speech_in, sequence ) ) {
                            dstar_tx_vocoder_submit_failure_count++;
                            dstar_tx_stream_offerVoice(
                                &tx_stream, _gmsk_mod, TX4_cb,
                                sequence, NULL, FALSE );
                        }
                    }
                    dstar_tx_partial_flushed = TRUE;
                }

                _dstar_collect_tx_ambe( &tx_stream );
                uint32 pending_vocoder =
                    aether_vocoder_pending_encode_requests();
                uint32 outstanding_vocoder =
                    aether_vocoder_encode_outstanding();
                if ( pending_vocoder > dstar_tx_vocoder_pending_max ) {
                    dstar_tx_vocoder_pending_max = pending_vocoder;
                }
                if ( dstar_tx_drain_initial_frames == 0U ) {
                    dstar_tx_drain_initial_frames =
                        dstar_tx_stream_pendingVoiceFrames( &tx_stream )
                        + outstanding_vocoder;
                }

                const uint64 drain_elapsed_us = now_ns > dstar_tx_drain_started_ns
                    ? ( now_ns - dstar_tx_drain_started_ns ) / 1000U
                    : 0U;
                if ( !dstar_tx_drain_budget_trimmed
                     && drain_elapsed_us >= DSTAR_TX_DRAIN_AUDIO_BUDGET_US
                     && ( outstanding_vocoder > 0U
                          || dstar_tx_stream_pendingVoiceFrames(
                                 &tx_stream ) > 0U ) ) {
                    _dstar_collect_tx_ambe( &tx_stream );
                    pending_vocoder = aether_vocoder_pending_encode_requests();
                    const uint32 queued_frames =
                        dstar_tx_stream_pendingVoiceFrames( &tx_stream );
                    output( "AETHER_DSTAR_DIAG tx_drain_budget_trim elapsed_us=%llu pending_vocoder=%u queued_frames=%u\n",
                            ( unsigned long long )drain_elapsed_us,
                            pending_vocoder,
                            queued_frames );
                    dstar_tx_drain_budget_trimmed = TRUE;
                    aether_vocoder_flush_lists();
                    dstar_tx_stream_discardPendingVoice( &tx_stream );
                    pending_vocoder = 0U;
                    outstanding_vocoder = 0U;
                }

                if ( outstanding_vocoder == 0U
                     && dstar_tx_stream_pendingVoiceFrames( &tx_stream ) == 0U ) {
                    const dstar_tx_stream_end_result end_result =
                        dstar_tx_stream_requestEnd(
                            &tx_stream, _gmsk_mod, TX4_cb );
                    if ( end_result == DSTAR_TX_STREAM_END_QUEUED
                         && dstar_transmit_state_markEndQueued(
                                &_transmit_state ) ) {
                        dstar_tx_tail_samples = dstar_tx_stream_pendingSamples(
                            &tx_stream, TX4_cb );
                        output( "AETHER_DSTAR_DIAG tx_end_pattern tx4_samples=%u drain_frames=%u\n",
                                dstar_tx_tail_samples,
                                dstar_tx_drain_initial_frames );
                        current_phase = DSTAR_TRANSMIT_ENDING;
                    }
                } else if ( tx_stream.phase == DSTAR_TX_STREAM_IDLE ) {
                    _dstar_finish_transmit_tail();
                    dstar_tx_tail_deadline_valid = FALSE;
                }
            }

            BOOL tail_packet_emitted = FALSE;
            do {
                buf_desc = _WaveformList_UnlinkHead();
                BOOL synthetic_tx_buffer = FALSE;

                const BOOL tail_due = !dstar_tx_tail_deadline_valid
                    || tail_wait_timed_out;
                current_phase = dstar_transmit_state_phase( &_transmit_state );
                if ( buf_desc == NULL
                     && !tail_packet_emitted
                     && tail_due
                     && ( current_phase == DSTAR_TRANSMIT_DRAINING
                          || current_phase == DSTAR_TRANSMIT_ENDING )
                     && tx_stream.phase != DSTAR_TX_STREAM_IDLE
                     && ( current_phase == DSTAR_TRANSMIT_DRAINING
                          || dstar_tx_stream_pendingSamples(
                                 &tx_stream, TX4_cb ) > 0U ) ) {
                    const digital_voice_stream_map * streams =
                        digital_voice_mode_registry_streams();
                    if ( streams->valid ) {
                        buf_desc = hal_BufferRequest(
                            PACKET_SAMPLES, sizeof( Complex ) );
                        if ( buf_desc != NULL ) {
                            buf_desc->stream_id = streams->tx_stream_in_id;
                            synthetic_tx_buffer = TRUE;
                        }
                    }
                    if ( buf_desc == NULL ) {
                        output( "AETHER_DV_ERROR tx_drain_buffer_unavailable streams_valid=%d\n",
                                streams->valid );
                        aether_vocoder_flush_lists();
                        dstar_tx_stream_reset( &tx_stream, TX4_cb );
                        sched_waveform_cancelTransmit();
                        dstar_tx_tail_deadline_valid = FALSE;
                    }
                }

                // if we got signalled, but there was no new data, something's wrong
                // and we'll just wait for the next packet
                if ( buf_desc == NULL ) {
                    //output( "We were signaled that there was another buffer descriptor, but there's not one here");
                    break;
                } else {
                    // convert the buffer to little endian
                    if ( !synthetic_tx_buffer ) {
                        _dsp_convertBufEndian( buf_desc );
                    }

                    //output(" \"Processed\" buffer stream id = 0x%08X\n", buf_desc->stream_id);

                    const digital_voice_stream_direction stream_direction =
                        digital_voice_mode_registry_stream_direction(
                            buf_desc->stream_id );
                    if ( stream_direction == DIGITAL_VOICE_STREAM_UNKNOWN ) {
                        output( "AETHER_DV_DIAG unknown_stream stream=0x%08X\n",
                                buf_desc->stream_id );
                        hal_BufferRelease( &buf_desc );
                        continue;
                    }

                    if ( stream_direction == DIGITAL_VOICE_STREAM_RX ) {
                        Complex rx_input[PACKET_SAMPLES];
                        memcpy( rx_input, buf_desc->buf_ptr, sizeof( rx_input ) );
                        dstar_rx_input_buffer_count++;
                        if ( _verbose_rx_idle_diag
                             && ( dstar_rx_input_buffer_count <= 5 || dstar_rx_input_buffer_count % 100 == 0 ) ) {
                            float rx_input_peak = 0.0f;
                            uint32 peak_sample_count = buf_desc->num_samples;
                            if ( peak_sample_count > PACKET_SAMPLES ) {
                                peak_sample_count = PACKET_SAMPLES;
                            }
                            for ( uint32 sample_idx = 0 ; sample_idx < peak_sample_count ; sample_idx++ ) {
                                const float sample_peak = _dstar_abs_float( rx_input[sample_idx].real );
                                if ( sample_peak > rx_input_peak ) {
                                    rx_input_peak = sample_peak;
                                }
                            }
                            output( "AETHER_DSTAR_DIAG rx_input_buffer count=%u stream=0x%08X samples=%u peak=%.3f initial=%d\n",
                                    dstar_rx_input_buffer_count,
                                    buf_desc->stream_id,
                                    buf_desc->num_samples,
                                    rx_input_peak,
                                    initial_rx );
                        }

                        const BOOL transmit_or_tail_enabled = _dstar_transmit_or_tail_enabled();
                        if ( transmit_or_tail_enabled ) {
                            memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof( Complex ) );
                            _dstar_record_rx_metrics( buf_desc );
                            emit_waveform_output( buf_desc );
                            _dstar_capture_rx_packet( buf_desc, rx_input );
                        } else {
                            const BOOL requested_rx_reset = _dstar_consume_receive_reset_request();
                            const BOOL reset_rx = initial_rx || requested_rx_reset;

                            /* Do not return stale speech while resetting the receive pipeline. */
                            if ( reset_rx ) {
                                RX3_cb->start = 0;
                                RX3_cb->end = 0;
                                RX4_cb->start = 0;
                                RX4_cb->end = 0;
                                dstar_rx_playout_started = FALSE;
                                dstar_rx_demod_sample_count = 0U;
                                dstar_rx_last_sync_sample = 0U;
                            }

                            /*
                             * The Flex waveform stream is turnaround-sensitive. Return speech
                             * already produced for earlier RF packets before demodulating this
                             * packet or touching the ThumbDV serial path. The copied RF samples
                             * remain available below after the output buffer is overwritten.
                             */
                            uint32 rx_playout_samples = cfbContains( RX4_cb );
                            if ( dstar_rx_playout_started
                                 && rx_playout_samples < PACKET_SAMPLES ) {
                                dstar_rx_playout_started = FALSE;
                                RX4_cb->start = 0;
                                RX4_cb->end = 0;
                                rx_playout_samples = 0U;
                            }
                            if ( !dstar_rx_playout_started
                                 && rx_playout_samples
                                     >= DSTAR_RX_PLAYOUT_PREBUFFER_SAMPLES ) {
                                dstar_rx_playout_started = TRUE;
                            }
                            const BOOL emitted_rx_samples = dstar_rx_playout_started;
                            if ( emitted_rx_samples ) {
                                for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                    fsample = cbReadFloat( RX4_cb );
                                    ( ( Complex * )buf_desc->buf_ptr )[i].real = fsample;
                                    ( ( Complex * )buf_desc->buf_ptr )[i].imag = fsample;
                                }
                            } else {
                                memset( buf_desc->buf_ptr, 0, PACKET_SAMPLES * sizeof( Complex ) );
                            }
                            _dstar_record_rx_metrics( buf_desc );
                            emit_waveform_output( buf_desc );
                            _dstar_capture_rx_packet( buf_desc, rx_input );
                            struct timespec rx_work_started = {0};
                            if ( _diag_rx_timing ) {
                                clock_gettime( CLOCK_MONOTONIC, &rx_work_started );
                            }
                            const BOOL verbose_diagnostics = _dstar_verbose();

                            if ( emitted_rx_samples ) {
                                dstar_rx_output_buffer_count++;
                                if ( _diag_rx_timing ) {
                                    dstar_rx_timing_output_packets++;
                                }
                                if ( verbose_diagnostics
                                     && ( dstar_rx_output_buffer_count <= 5
                                          || dstar_rx_output_buffer_count % 100 == 0 ) ) {
                                    output( "AETHER_DSTAR_DIAG rx_output_buffer count=%u stream=0x%08X\n",
                                            dstar_rx_output_buffer_count,
                                            buf_desc->stream_id );
                                }
                            } else {
                                dstar_rx_zero_output_buffer_count++;
                                if ( _diag_rx_timing ) {
                                    dstar_rx_timing_zero_packets++;
                                }
                                if ( _verbose_rx_idle_diag
                                     && ( dstar_rx_zero_output_buffer_count <= 5
                                          || dstar_rx_zero_output_buffer_count % 100 == 0 ) ) {
                                    output( "AETHER_DSTAR_DIAG rx_zero_output count=%u stream=0x%08X rx4_samples=%u\n",
                                            dstar_rx_zero_output_buffer_count,
                                            buf_desc->stream_id,
                                            cfbContains( RX4_cb ) );
                                }
                            }

                            if ( reset_rx ) {
                                memset( float_in_24k, 0, MEM_24 * sizeof( float ) );
                                memset( float_in_8k, 0, MEM_8 * sizeof( float ) );
                                aether_vocoder_flush_lists();
                                _dstar_reset_rx_machine();
                                output( "AETHER_DSTAR_DIAG rx_reset initial=%d requested=%d\n",
                                        initial_rx,
                                        requested_rx_reset );
                            }

                            initial_rx = FALSE;
                            if ( tx_stream.phase != DSTAR_TX_STREAM_IDLE ) {
                                TX1_cb->start = TX1_cb->end = 0U;
                                TX2_cb->start = TX2_cb->end = 0U;
                                TX3_cb->start = TX3_cb->end = 0U;
                                dstar_tx_stream_reset( &tx_stream, TX4_cb );
                                gmsk_resetMODFilter( _gmsk_mod );
                            }

                            enum DEMOD_STATE state = DEMOD_UNKNOWN;
                            for ( i = 0 ; i < PACKET_SAMPLES ; i++ ) {
                                dstar_rx_demod_sample_count++;
                                state = gmsk_decode( _gmsk_demod, rx_input[i].real );

                                if ( state == DEMOD_UNKNOWN ) {
                                    continue;
                                }
                                aether_dstar_rx_event event;
                                const crdv_result rx_result =
                                    aether_dstar_protocol_push_rx_bit(
                                        _dstar,
                                        state == DEMOD_TRUE ? 1U : 0U,
                                        &event );
                                if ( rx_result != CRDV_OK ) {
                                    output( "AETHER_DSTAR_ERROR receive parser rejected bit result=%d\n",
                                            rx_result );
                                    _dstar_reset_rx_machine();
                                    continue;
                                }

                                uint32 receive_slice = 0U;
                                if ( event.header || event.message || event.end ) {
                                    receive_slice = _dstar_slice();
                                }
                                if ( event.header ) {
                                    _dstar_send_rx_header(
                                        receive_slice, &event.header_fields );
                                    if ( !event.voice ) {
                                        dstar_rx_last_sync_sample = 0U;
                                    }
                                }
                                if ( event.message ) {
                                    _dstar_send_rx_message(
                                        receive_slice, event.message_text );
                                }
                                if ( event.end ) {
                                    _dstar_send_rx_end( receive_slice );
                                    dstar_rx_last_sync_sample = 0U;
                                }

                                const BOOL accepted_sync = event.sync_event
                                    && ( event.sync.kind == CRDV_SYNC_EXACT
                                         || event.sync.kind
                                             == CRDV_SYNC_TOLERANT
                                         || event.sync.kind
                                             == CRDV_SYNC_REACQUIRED_EARLY
                                         || event.sync.kind
                                             == CRDV_SYNC_REACQUIRED_LATE );
                                if ( accepted_sync ) {
                                    dstar_rx_data_sync_count++;
                                    if ( dstar_rx_last_sync_sample != 0U ) {
                                        const uint64 interval =
                                            dstar_rx_demod_sample_count
                                            - dstar_rx_last_sync_sample;
                                        dstar_waveform_metrics_record_sync_interval(
                                            &_rx_waveform_metrics,
                                            interval,
                                            DSTAR_RX_SYNC_INTERVAL_SAMPLES,
                                            PACKET_SAMPLES,
                                            2U );
                                        if ( _diag_sync_timing ) {
                                            const int64 error =
                                                ( int64 )interval
                                                - DSTAR_RX_SYNC_INTERVAL_SAMPLES;
                                            output( "AETHER_DSTAR_DIAG rx_sync count=%u interval_samples=%llu error_samples=%lld kind=%d offset_bits=%d hamming=%u\n",
                                                    dstar_rx_data_sync_count,
                                                    ( unsigned long long )interval,
                                                    ( long long )error,
                                                    event.sync.kind,
                                                    event.sync.bit_offset,
                                                    ( unsigned int )event.sync.hamming_distance );
                                        }
                                    }
                                    dstar_rx_last_sync_sample =
                                        dstar_rx_demod_sample_count;
                                }

                                if ( event.voice ) {
                                    dstar_rx_ambe_frame_count++;
                                    if ( verbose_diagnostics
                                         && ( dstar_rx_ambe_frame_count <= 5
                                              || dstar_rx_ambe_frame_count % 50 == 0 ) ) {
                                        output( "AETHER_DSTAR_DIAG rx_ambe_frame count=%u\n",
                                                dstar_rx_ambe_frame_count );
                                    }
                                    struct timespec vocoder_started = {0};
                                    if ( _diag_rx_timing ) {
                                        clock_gettime( CLOCK_MONOTONIC, &vocoder_started );
                                    }
                                    pthread_rwlock_rdlock( &_dv_serial_handle_lock );
                                    if ( _dv_serial_handle != NULL ) {
                                        aether_vocoder_decode(
                                            _dv_serial_handle,
                                            event.ambe,
                                            CRDV_AMBE_BYTES );
                                    }
                                    pthread_rwlock_unlock( &_dv_serial_handle_lock );
                                    if ( _diag_rx_timing ) {
                                        struct timespec vocoder_finished;
                                        clock_gettime( CLOCK_MONOTONIC, &vocoder_finished );
                                        const uint64 vocoder_us = _dstar_elapsed_usec(
                                            &vocoder_started, &vocoder_finished );
                                        dstar_rx_timing_vocoder_calls++;
                                        dstar_rx_timing_vocoder_total_us += vocoder_us;
                                        if ( vocoder_us > dstar_rx_timing_vocoder_max_us ) {
                                            dstar_rx_timing_vocoder_max_us = vocoder_us;
                                        }
                                    }
                                }
                            }

                            for ( uint32 decoded_block = 0U;
                                  decoded_block < DSTAR_MAX_DECODED_AUDIO_BLOCKS_PER_PACKET;
                                  decoded_block++ ) {
                                nout = aether_vocoder_unlink_audio( speech_out );
                                if ( nout <= 0 ) {
                                    break;
                                }

                                dstar_rx_audio_block_count++;
                                if ( _diag_rx_timing ) {
                                    dstar_rx_timing_audio_blocks++;
                                }
                                if ( verbose_diagnostics
                                     && ( dstar_rx_audio_block_count <= 5
                                          || dstar_rx_audio_block_count % 50 == 0 ) ) {
                                    output( "AETHER_DSTAR_DIAG rx_audio_block count=%u samples=%d\n",
                                            dstar_rx_audio_block_count,
                                            nout );
                                }

                                for ( uint32 j = 0U; j < ( uint32 )nout; j++ ) {
                                    cbWriteShort( RX3_cb, speech_out[j] );
                                }
                            }

                            if ( csbContains( RX3_cb ) >= DV_PACKET_SAMPLES ) {
                                for ( i = 0 ; i < DV_PACKET_SAMPLES ; i++ ) {
                                    float_in_8k[i + MEM_8] =
                                        ( float )( cbReadShort( RX3_cb ) / SCALE_RX_OUT );
                                }

                                fdmdv_8_to_24(
                                    float_out_24k, &float_in_8k[MEM_8], DV_PACKET_SAMPLES );
                                for ( i = 0 ; i < DV_PACKET_SAMPLES * DECIMATION_FACTOR ; i++ ) {
                                    cbWriteFloat( RX4_cb, float_out_24k[i] );
                                }
                            }

                            if ( _diag_rx_timing ) {
                                struct timespec rx_work_finished;
                                clock_gettime( CLOCK_MONOTONIC, &rx_work_finished );
                                const uint64 work_us = _dstar_elapsed_usec(
                                    &rx_work_started, &rx_work_finished );
                                const uint32 queue_depth = _WaveformList_Depth();
                                dstar_rx_timing_packet_count++;
                                dstar_rx_timing_total_us += work_us;
                                if ( work_us > dstar_rx_timing_max_us ) {
                                    dstar_rx_timing_max_us = work_us;
                                }
                                if ( work_us > DSTAR_RX_PACKET_BUDGET_US ) {
                                    dstar_rx_timing_over_budget_count++;
                                }
                                if ( queue_depth > dstar_rx_timing_queue_max ) {
                                    dstar_rx_timing_queue_max = queue_depth;
                                }

                                if ( dstar_rx_timing_packet_count >= DSTAR_RX_TIMING_REPORT_PACKETS ) {
                                    const double mean_us =
                                        ( double )dstar_rx_timing_total_us
                                        / ( double )dstar_rx_timing_packet_count;
                                    const double vocoder_mean_us =
                                        dstar_rx_timing_vocoder_calls == 0U
                                            ? 0.0
                                            : ( double )dstar_rx_timing_vocoder_total_us
                                                / ( double )dstar_rx_timing_vocoder_calls;
                                    output( "AETHER_DSTAR_DIAG rx_work packets=%u mean_us=%.1f max_us=%llu over_budget=%u queue_max=%u vocoder_calls=%u audio_blocks=%u output_packets=%u zero_packets=%u rx4_queued=%u vocoder_mean_us=%.1f vocoder_max_us=%llu\n",
                                            dstar_rx_timing_packet_count,
                                            mean_us,
                                            ( unsigned long long )dstar_rx_timing_max_us,
                                            dstar_rx_timing_over_budget_count,
                                            dstar_rx_timing_queue_max,
                                            dstar_rx_timing_vocoder_calls,
                                            dstar_rx_timing_audio_blocks,
                                            dstar_rx_timing_output_packets,
                                            dstar_rx_timing_zero_packets,
                                            cfbContains( RX4_cb ),
                                            vocoder_mean_us,
                                            ( unsigned long long )dstar_rx_timing_vocoder_max_us );
                                    dstar_rx_timing_packet_count = 0U;
                                    dstar_rx_timing_over_budget_count = 0U;
                                    dstar_rx_timing_queue_max = 0U;
                                    dstar_rx_timing_vocoder_calls = 0U;
                                    dstar_rx_timing_audio_blocks = 0U;
                                    dstar_rx_timing_output_packets = 0U;
                                    dstar_rx_timing_zero_packets = 0U;
                                    dstar_rx_timing_total_us = 0U;
                                    dstar_rx_timing_max_us = 0U;
                                    dstar_rx_timing_vocoder_total_us = 0U;
                                    dstar_rx_timing_vocoder_max_us = 0U;
                                }
                            }
                        }

                    } else if ( stream_direction == DIGITAL_VOICE_STREAM_TX ) {
                        const enum dstar_transmit_phase transmit_phase =
                            dstar_transmit_state_phase( &_transmit_state );
                        const BOOL transmit_enabled =
                            transmit_phase != DSTAR_TRANSMIT_IDLE;
                        const BOOL tx_verbose_diagnostics = _dstar_verbose();
                        dstar_tx_input_buffer_count++;
                        if ( tx_verbose_diagnostics
                             && ( dstar_tx_input_buffer_count <= 5U
                                  || dstar_tx_input_buffer_count % 100U == 0U ) ) {
                            output( "AETHER_DSTAR_DIAG tx_input_buffer count=%u stream=0x%08X samples=%u phase=%d synthetic=%d enabled=%d\n",
                                    dstar_tx_input_buffer_count,
                                    buf_desc->stream_id,
                                    buf_desc->num_samples,
                                    tx_stream.phase,
                                    synthetic_tx_buffer,
                                    transmit_enabled );
                        }

                        if ( !transmit_enabled ) {
                            TX1_cb->start = TX1_cb->end = 0U;
                            TX2_cb->start = TX2_cb->end = 0U;
                            TX3_cb->start = TX3_cb->end = 0U;
                            if ( tx_stream.phase != DSTAR_TX_STREAM_IDLE ) {
                                aether_vocoder_flush_lists();
                                dstar_tx_stream_reset( &tx_stream, TX4_cb );
                                gmsk_resetMODFilter( _gmsk_mod );
                            }
                        } else {
                            if ( tx_stream.phase == DSTAR_TX_STREAM_IDLE
                                 && transmit_phase == DSTAR_TRANSMIT_ACTIVE ) {
                                TX1_cb->start = TX1_cb->end = 0U;
                                TX2_cb->start = TX2_cb->end = 0U;
                                TX3_cb->start = TX3_cb->end = 0U;
                                memset( tx_float_in_24k, 0,
                                        MEM_24 * sizeof( float ) );
                                aether_vocoder_flush_lists();
                                gmsk_resetMODFilter( _gmsk_mod );
                                aether_dstar_tx_snapshot snapshot;
                                if ( !_dstar_copy_tx_snapshot( &snapshot )
                                     || !dstar_tx_stream_begin(
                                            &tx_stream,
                                            &snapshot,
                                            _gmsk_mod,
                                            TX4_cb ) ) {
                                    output( "AETHER_DSTAR_ERROR unable to start transmit bitstream\n" );
                                    sched_waveform_cancelTransmit();
                                }
                                dstar_tx_voice_frame_count = 0U;
                                dstar_tx_null_voice_frame_count = 0U;
                                dstar_tx_pcm_clip_count = 0U;
                                dstar_tx_pcm_invalid_count = 0U;
                                dstar_tx_send_failure_count = 0U;
                                dstar_tx_vocoder_submit_failure_count = 0U;
                                dstar_tx_vocoder_pending_max = 0U;
                                dstar_tx_drain_timeout_count = 0U;
                                dstar_tx_drain_initial_frames = 0U;
                                dstar_tx_tail_samples = 0U;
                                dstar_tx_tail_started_ns = 0U;
                                dstar_tx_drain_started_ns = 0U;
                                dstar_tx_next_sequence = 0U;
                                dstar_tx_partial_flushed = FALSE;
                                dstar_tx_drain_budget_trimmed = FALSE;
                                dstar_waveform_metrics_reset(
                                    &_tx_waveform_metrics );
                            }

                            initial_rx = TRUE;
                            if ( transmit_phase == DSTAR_TRANSMIT_ACTIVE
                                 && !synthetic_tx_buffer ) {
                                _dstar_collect_tx_ambe( &tx_stream );
                                uint32 input_samples = buf_desc->num_samples;
                                if ( input_samples > PACKET_SAMPLES ) {
                                    input_samples = PACKET_SAMPLES;
                                }
                                for ( uint32 sample_index = 0U;
                                      sample_index < input_samples;
                                      sample_index++ ) {
                                    cbWriteFloat(
                                        TX1_cb,
                                        ( ( Complex * )buf_desc->buf_ptr )
                                            [sample_index].real );
                                }

                                while ( _dstar_prepare_tx_speech(
                                            tx_float_in_24k,
                                            tx_float_out_8k,
                                            speech_in,
                                            FALSE,
                                            &dstar_tx_pcm_clip_count,
                                            &dstar_tx_pcm_invalid_count ) ) {
                                    const uint64 sequence =
                                        dstar_tx_next_sequence++;
                                    const BOOL submitted =
                                        _dstar_submit_tx_speech(
                                            &tx_stream, speech_in, sequence );
                                    if ( !submitted ) {
                                        dstar_tx_vocoder_submit_failure_count++;
                                        dstar_tx_stream_offerVoice(
                                            &tx_stream,
                                            _gmsk_mod,
                                            TX4_cb,
                                            sequence,
                                            NULL,
                                            FALSE );
                                    }
                                    dstar_tx_voice_frame_count++;
                                    if ( tx_verbose_diagnostics
                                         && ( dstar_tx_voice_frame_count <= 5U
                                              || dstar_tx_voice_frame_count
                                                  % 50U == 0U ) ) {
                                        output( "AETHER_DSTAR_DIAG tx_voice_frame count=%u sequence=%llu submitted=%d header=%d ambe_queued=%u pending_vocoder=%u tx4_samples=%u\n",
                                                dstar_tx_voice_frame_count,
                                                ( unsigned long long )sequence,
                                                submitted,
                                                tx_stream.phase
                                                    == DSTAR_TX_STREAM_HEADER,
                                                dstar_tx_stream_pendingVoiceFrames(
                                                    &tx_stream ),
                                                aether_vocoder_pending_encode_requests(),
                                                cfbContains( TX4_cb ) );
                                    }
                                }
                                _dstar_collect_tx_ambe( &tx_stream );
                                const uint32 pending_vocoder =
                                    aether_vocoder_pending_encode_requests();
                                if ( pending_vocoder
                                     > dstar_tx_vocoder_pending_max ) {
                                    dstar_tx_vocoder_pending_max =
                                        pending_vocoder;
                                }
                            }

                            float tx_packet[PACKET_SAMPLES] = {0.0f};
                            const uint32 output_samples =
                                dstar_tx_stream_readPacket(
                                    &tx_stream,
                                    _gmsk_mod,
                                    TX4_cb,
                                    tx_packet,
                                    PACKET_SAMPLES );
                            memset( buf_desc->buf_ptr, 0,
                                    PACKET_SAMPLES * sizeof( Complex ) );
                            float tx_output_peak = 0.0f;
                            for ( uint32 sample_index = 0U;
                                  sample_index < output_samples;
                                  sample_index++ ) {
                                fsample = _dstar_scale_tx_sample(
                                    tx_packet[sample_index] );
                                const float abs_sample =
                                    _dstar_abs_float( fsample );
                                if ( abs_sample > tx_output_peak ) {
                                    tx_output_peak = abs_sample;
                                }
                                ( ( Complex * )buf_desc->buf_ptr )
                                    [sample_index].real = fsample;
                                ( ( Complex * )buf_desc->buf_ptr )
                                    [sample_index].imag = fsample;
                            }

                            if ( output_samples > 0U ) {
                                _dstar_capture_real_samples(
                                    &_tx_sample_capture,
                                    &_tx_sample_capture_count,
                                    ( const Complex * )buf_desc->buf_ptr,
                                    buf_desc->num_samples );
                                if ( !emit_waveform_output( buf_desc ) ) {
                                    dstar_tx_send_failure_count++;
                                }
                                dstar_tx_output_buffer_count++;
                                if ( tx_verbose_diagnostics
                                     && ( dstar_tx_output_buffer_count <= 5U
                                          || dstar_tx_output_buffer_count
                                              % 100U == 0U ) ) {
                                    output( "AETHER_DSTAR_DIAG tx_output_buffer count=%u stream=0x%08X peak=%.3f gain=%.3f tx4_samples=%u\n",
                                            dstar_tx_output_buffer_count,
                                            buf_desc->stream_id,
                                            tx_output_peak,
                                            _tx_sample_gain,
                                            cfbContains( TX4_cb ) );
                                }
                            } else {
                                dstar_tx_zero_output_buffer_count++;
                            }

                            dstar_tx_null_voice_frame_count =
                                tx_stream.null_frame_count;
                            if ( !synthetic_tx_buffer ) {
                                _dstar_record_tx_packet_metrics(
                                    buf_desc,
                                    tx_stream.queue_max_samples );
                            }
                            if ( ( transmit_phase == DSTAR_TRANSMIT_DRAINING
                                   || transmit_phase == DSTAR_TRANSMIT_ENDING )
                                 && output_samples > 0U ) {
                                tail_packet_emitted = TRUE;
                                if ( transmit_phase == DSTAR_TRANSMIT_ENDING
                                     && dstar_tx_stream_finished(
                                         &tx_stream, TX4_cb ) ) {
                                    const uint64 tail_finished_ns =
                                        _dstar_monotonic_ns();
                                    const uint64 tail_us =
                                        dstar_tx_tail_started_ns > 0U
                                        && tail_finished_ns
                                            > dstar_tx_tail_started_ns
                                            ? ( tail_finished_ns
                                                - dstar_tx_tail_started_ns )
                                                / 1000U
                                            : 0U;
                                    dstar_waveform_metrics_record_tx_activity(
                                        &_tx_waveform_metrics,
                                        dstar_tx_null_voice_frame_count,
                                        dstar_tx_pcm_clip_count,
                                        dstar_tx_pcm_invalid_count,
                                        dstar_tx_send_failure_count,
                                        tx_stream.queue_max_samples );
                                    dstar_waveform_metrics_record_tail(
                                        &_tx_waveform_metrics,
                                        dstar_tx_tail_samples,
                                        tail_us );
                                    const uint32 pre_roll_delay_ms =
                                        ( uint32 )(
                                            ( tx_stream.snapshot.preamble_bits
                                              + 15U
                                              + CRDV_PROTECTED_BITS )
                                            * 1000U
                                            / AETHER_DSTAR_SYMBOL_RATE );
                                    dstar_waveform_metrics_record_tx_pipeline(
                                        &_tx_waveform_metrics,
                                        tx_stream.pre_roll_frames,
                                        pre_roll_delay_ms,
                                        tx_stream.queue_max_frames,
                                        tx_stream.underflow_count,
                                        tx_stream.overflow_count,
                                        tx_stream.sequence_error_count,
                                        dstar_tx_vocoder_submit_failure_count,
                                        dstar_tx_vocoder_pending_max,
                                        dstar_tx_drain_initial_frames,
                                        dstar_tx_drain_timeout_count,
                                        tx_stream.discarded_frame_count );
                                    dstar_waveform_metric_snapshot snapshot;
                                    if ( dstar_waveform_metrics_finish_window(
                                             &_tx_waveform_metrics,
                                             &snapshot ) ) {
                                        _dstar_output_tx_metrics( &snapshot );
                                    }
                                    output( "AETHER_DSTAR_DIAG tx_tail_flushed packets=%u null_frames=%u pcm_clips=%u pcm_invalid=%u send_failures=%u submit_failures=%u sample_queue_max=%u ambe_queue_max=%u ambe_underflows=%u ambe_overflows=%u sequence_errors=%u preroll_frames=%u vocoder_pending_max=%u drain_frames=%u drain_timeouts=%u drain_us=%llu\n",
                                            dstar_tx_output_buffer_count,
                                            dstar_tx_null_voice_frame_count,
                                            dstar_tx_pcm_clip_count,
                                            dstar_tx_pcm_invalid_count,
                                            dstar_tx_send_failure_count,
                                            dstar_tx_vocoder_submit_failure_count,
                                            tx_stream.queue_max_samples,
                                            tx_stream.queue_max_frames,
                                            tx_stream.underflow_count,
                                            tx_stream.overflow_count,
                                            tx_stream.sequence_error_count,
                                            tx_stream.pre_roll_frames,
                                            dstar_tx_vocoder_pending_max,
                                            dstar_tx_drain_initial_frames,
                                            dstar_tx_drain_timeout_count,
                                            ( unsigned long long )tail_us );
                                    aether_vocoder_flush_lists();
                                    _dstar_finish_transmit_tail();
                                    dstar_tx_stream_reset(
                                        &tx_stream, TX4_cb );
                                    TX1_cb->start = TX1_cb->end = 0U;
                                    TX2_cb->start = TX2_cb->end = 0U;
                                    TX3_cb->start = TX3_cb->end = 0U;
                                    dstar_tx_tail_deadline_valid = FALSE;
                                } else {
                                    _dstar_set_deadline_after_usec(
                                        &dstar_tx_tail_deadline,
                                        dstar_tx_output_packetIntervalUsec(
                                            PACKET_SAMPLES,
                                            AETHER_DSTAR_SAMPLE_RATE ) );
                                    dstar_tx_tail_deadline_valid = TRUE;
                                }
                            }
                        }
                    }

                    hal_BufferRelease( &buf_desc );
                    if ( tail_packet_emitted ) {
                        break;
                    }
                }
            } while ( 1 ); // Seems infinite loop but will exit once there are no longer any buffers linked in _Waveformlist
        }
    }

    _waveform_thread_abort = TRUE;

    gmsk_destroyDemodulator( _gmsk_demod );
    gmsk_destroyModulator( _gmsk_mod );
    if ( _rx_sample_capture != NULL ) {
        output( "AETHER_DSTAR_DIAG sample_capture_close direction=rx samples=%llu\n",
                ( unsigned long long )_rx_sample_capture_count );
        fclose( _rx_sample_capture );
        _rx_sample_capture = NULL;
    }
    if ( _tx_sample_capture != NULL ) {
        output( "AETHER_DSTAR_DIAG sample_capture_close direction=tx samples=%llu\n",
                ( unsigned long long )_tx_sample_capture_count );
        fclose( _tx_sample_capture );
        _tx_sample_capture = NULL;
    }
    if ( _rx_metadata_capture != NULL ) {
        output( "AETHER_DSTAR_DIAG metadata_capture_close direction=rx packets=%llu samples=%llu\n",
                ( unsigned long long )_rx_metadata_packet_count,
                ( unsigned long long )_rx_input_sample_offset );
        fclose( _rx_metadata_capture );
        _rx_metadata_capture = NULL;
    }

    return NULL;
}

void sched_waveform_Init( void ) {

    pthread_rwlock_init( &_dv_serial_handle_lock, NULL );
    pthread_rwlock_init( &_dstar_config_lock, NULL );
    _dstar = &_dstar_storage;
    aether_dstar_protocol_init( _dstar );
    dstar_waveform_metrics_reset( &_rx_waveform_metrics );
    dstar_waveform_metrics_reset( &_tx_waveform_metrics );
    _dstar->verbose = _dstar_env_bool(
        "AETHER_DSTAR_VERBOSE_DIAG", FALSE );
    _diag_rx_timing = _dstar_env_bool( "AETHER_DSTAR_DIAG_RX_TIMING", FALSE );
    _diag_rx_turnaround = _dstar_env_bool(
        "AETHER_DSTAR_DIAG_RX_TURNAROUND", FALSE );
    _diag_sync_timing = _dstar_env_bool(
        "AETHER_DSTAR_DIAG_SYNC_TIMING", FALSE );
    const uint32 sync_max_errors = _dstar_env_uint(
        "AETHER_DSTAR_RX_SYNC_MAX_ERRORS",
        DSTAR_RX_SYNC_MAX_ERRORS_DEFAULT,
        0U,
        24U );
    const uint32 sync_miss_limit = _dstar_env_uint(
        "AETHER_DSTAR_RX_SYNC_MISS_LIMIT",
        DSTAR_RX_SYNC_MISS_LIMIT_DEFAULT,
        0U,
        255U );
    const uint32 sync_realign_bits = _dstar_env_uint(
        "AETHER_DSTAR_RX_SYNC_REALIGN_BITS",
        DSTAR_RX_SYNC_REALIGN_BITS_DEFAULT,
        0U,
        24U );
    crdv_receive_sync_policy sync_policy;
    memset( &sync_policy, 0, sizeof( sync_policy ) );
    sync_policy.max_hamming_distance = ( uint8_t )sync_max_errors;
    sync_policy.consecutive_miss_limit = ( uint8_t )sync_miss_limit;
    sync_policy.sliding_reacquisition = sync_realign_bits > 0U;
    sync_policy.max_realign_bits = ( uint8_t )sync_realign_bits;
    if ( aether_dstar_protocol_set_sync_policy(
             _dstar, &sync_policy ) != CRDV_OK ) {
        output( "AETHER_DSTAR_ERROR invalid receive synchronization policy\n" );
        exit( 1 );
    }

    const char * mycall = getenv( "AETHER_DSTAR_MYCALL" );
    const char * suffix = getenv( "AETHER_DSTAR_MYCALL_SUFFIX" );
    const char * urcall = getenv( "AETHER_DSTAR_URCALL" );
    const char * rpt1 = getenv( "AETHER_DSTAR_RPT1" );
    const char * rpt2 = getenv( "AETHER_DSTAR_RPT2" );
    sched_waveform_configureDStar(
        0U,
        mycall,
        suffix,
        urcall != NULL ? urcall : "CQCQCQ",
        rpt1 != NULL ? rpt1 : "DIRECT",
        rpt2 != NULL ? rpt2 : "DIRECT",
        getenv( "AETHER_DSTAR_MESSAGE" ) );

    _gmsk_demod = gmsk_createDemodulator();
    _gmsk_mod = gmsk_createModulator();
    _rx_sample_capture = _dstar_open_sample_capture( "AETHER_DSTAR_CAPTURE_RX_PATH" );
    _tx_sample_capture = _dstar_open_sample_capture( "AETHER_DSTAR_CAPTURE_TX_PATH" );
    _rx_metadata_capture = _dstar_open_metadata_capture(
        "AETHER_DSTAR_CAPTURE_RX_META_PATH" );
    _rx_sample_capture_count = 0U;
    _tx_sample_capture_count = 0U;
    _rx_input_sample_offset = 0U;
    _rx_metadata_packet_count = 0U;
    _gmsk_mod->m_invert = _dstar_env_bool( "AETHER_DSTAR_TX_INVERT", TRUE );
    _force_null_tx_ambe = _dstar_env_bool( "AETHER_DSTAR_TX_NULL_AMBE", FALSE );
    _tx_sample_gain = _dstar_env_float( "AETHER_DSTAR_TX_GAIN", 1.0f, 0.05f, 2.0f );
    const uint32 tx_preamble_ms = _dstar_env_uint(
        "AETHER_DSTAR_TX_PREAMBLE_MS", 250U, 14U, 1000U );
    aether_dstar_protocol_set_preamble(
        _dstar,
        ( size_t )tx_preamble_ms * AETHER_DSTAR_SYMBOL_RATE / 1000U );
    _verbose_rx_idle_diag = _dstar_env_bool( "AETHER_DSTAR_VERBOSE_RX_IDLE_DIAG", FALSE );
    output( "AETHER_DSTAR_DIAG tx_invert=%d tx_gain=%.3f tx_preamble_bits=%zu force_null_tx_ambe=%d verbose_rx_idle_diag=%d verbose_diag=%d rx_timing_diag=%d rx_turnaround_diag=%d sync_timing_diag=%d sync_max_errors=%u sync_miss_limit=%u sync_realign_bits=%u\n",
            _gmsk_mod->m_invert,
            _tx_sample_gain,
            _dstar->preamble_bits,
            _force_null_tx_ambe,
            _verbose_rx_idle_diag,
            _dstar->verbose,
            _diag_rx_timing,
            _diag_rx_turnaround,
            _diag_sync_timing,
            sync_max_errors,
            sync_miss_limit,
            sync_realign_bits );

    if ( !aether_buffer_queue_init(
             &_waveform_queue, DSTAR_WAVEFORM_QUEUE_LIMIT ) ) {
        output( "AETHER_DV_ERROR unable to initialize bounded waveform queue\n" );
        exit( 1 );
    }

    sem_init( &sched_waveform_sem, 0, 0 );

    if ( pthread_create(
             &_waveform_thread, NULL, &_sched_waveform_thread, NULL ) != 0 ) {
        output( "AETHER_DV_ERROR unable to start waveform scheduler thread\n" );
        exit( 1 );
    }

    struct sched_param fifo_param;
    fifo_param.sched_priority = 30;
    pthread_setschedparam( _waveform_thread, SCHED_FIFO, &fifo_param );
}

void sched_waveformThreadExit() {
    _waveform_thread_abort = TRUE;
    sem_post( &sched_waveform_sem );
}
