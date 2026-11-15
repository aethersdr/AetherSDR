///*!   \file thumbdv.c
// *    \brief Functions required to communicate and decode packets from ThumbDV
// *
// *    \copyright  Copyright 2012-2014 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 26-MAY-2015
// *    \author     Ed Gonzalez
// *
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

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/prctl.h>

#include <netinet/in.h>

#include "common.h"
#include "datatypes.h"
#include "hal_buffer.h"

#include "vita_output.h"
#include "thumbDV.h"
#include "sched_waveform.h"
#include "ftd2xx.h"

#define AMBE3000_HEADER_LEN     4U
#define AMBE3000_START_BYTE     0x61U

#define AMBE3000_SPEECHD_HEADER_LEN 3U

#define AMBE3000_CTRL_PKT_TYPE      0x00
#define AMBE3000_SPEECH_PKT_TYPE    0x02
#define AMBE3000_CHAN_PKT_TYPE      0x01

#define BUFFER_LENGTH           400U
#define THUMBDV_MAX_PACKET_LEN  2048U
#define THUMBDV_DSTAR_PCM_SAMPLES 160U
#define THUMBDV_DSTAR_AMBE_BYTES 9U

static pthread_t _read_thread;
static pthread_t _connect_thread;
BOOL _readThreadAbort = FALSE;
BOOL _connectThreadAbort = FALSE;

static uint32 _buffering_target = 0;
static uint32 _encode_buffering_target = 0;

static pthread_rwlock_t _encoded_list_lock;
static BufferDescriptor _encoded_root;
static BOOL _encoded_buffering = TRUE;
static uint32 _encoded_count = 0;

static pthread_rwlock_t _decoded_list_lock;
static BufferDescriptor _decoded_root;
static BOOL _decoded_buffering = TRUE;
static uint32 _decoded_count = 0;

#define THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH 256U

typedef struct _thumbdv_request {
    uint32 generation;
    uint64 correlation_id;
} thumbdv_request;

typedef struct _thumbdv_request_generation_queue {
    thumbdv_request values[THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH];
    uint32 head;
    uint32 count;
    uint32 current;
} thumbdv_request_generation_queue;

static pthread_mutex_t _request_generation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _read_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t _serial_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static thumbdv_request_generation_queue _encoded_requests;
static thumbdv_request_generation_queue _decoded_requests;
static BOOL _serial_fault_pending = FALSE;
static char _serial_fault_detail[512];

static sem_t _read_sem;
static unsigned char _last_control_field = 0xffU;
#ifdef _MSC_VER
static __declspec(thread) char _thumbDV_last_error[512] = "Unknown ThumbDV error";
#else
static _Thread_local char _thumbDV_last_error[512] = "Unknown ThumbDV error";
#endif
static BOOL _latency_timer_notice_emitted = FALSE;

//static void * _thumbDV_readThread( void * param );

BOOL allowedToRead = TRUE;

static void _thumbDVSetReadAllowed( BOOL allowed )
{
    pthread_mutex_lock( &_read_state_lock );
    allowedToRead = allowed;
    pthread_mutex_unlock( &_read_state_lock );
}

static BOOL _thumbDVReadAllowed( void )
{
    pthread_mutex_lock( &_read_state_lock );
    const BOOL allowed = allowedToRead;
    pthread_mutex_unlock( &_read_state_lock );
    return allowed;
}

static void _thumbDVSetLastError( const char * format, ... )
{
    va_list args;
    va_start( args, format );
    vsnprintf( _thumbDV_last_error, sizeof( _thumbDV_last_error ), format, args );
    va_end( args );
}

const char * thumbDV_lastError( void )
{
    return _thumbDV_last_error;
}

static void _thumbDVReportSerialFault( const char * detail )
{
    pthread_mutex_lock( &_serial_fault_lock );
    _serial_fault_pending = TRUE;
    snprintf( _serial_fault_detail,
              sizeof( _serial_fault_detail ),
              "%s",
              detail != NULL && detail[0] != '\0'
                  ? detail
                  : "DV3000 serial parser failed" );
    pthread_mutex_unlock( &_serial_fault_lock );
}

static BOOL _thumbDVTakeSerialFault( char * detail, size_t detail_size )
{
    pthread_mutex_lock( &_serial_fault_lock );
    const BOOL pending = _serial_fault_pending;
    if ( pending && detail != NULL && detail_size > 0U ) {
        snprintf( detail, detail_size, "%s", _serial_fault_detail );
    }
    if ( pending ) {
        _serial_fault_pending = FALSE;
        _serial_fault_detail[0] = '\0';
    }
    pthread_mutex_unlock( &_serial_fault_lock );
    return pending;
}

static void _thumbDVRequestGenerationEnqueue(
    thumbdv_request_generation_queue * queue,
    uint64 correlation_id ) {
    pthread_mutex_lock( &_request_generation_lock );
    if ( queue->count == THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH ) {
        queue->head = ( queue->head + 1U ) % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
        queue->count--;
        output( ANSI_YELLOW "ThumbDV: request generation queue overflow\n" ANSI_WHITE );
    }
    const uint32 tail = ( queue->head + queue->count ) % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
    queue->values[tail].generation = queue->current;
    queue->values[tail].correlation_id = correlation_id;
    queue->count++;
    pthread_mutex_unlock( &_request_generation_lock );
}

static BOOL _thumbDVConsumeResponse(
    thumbdv_request_generation_queue * queue,
    uint64 * correlation_id ) {
    BOOL current = FALSE;
    pthread_mutex_lock( &_request_generation_lock );
    if ( queue->count > 0U ) {
        const thumbdv_request request = queue->values[queue->head];
        queue->head = ( queue->head + 1U ) % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
        queue->count--;
        current = request.generation == queue->current;
        if ( current && correlation_id != NULL ) {
            *correlation_id = request.correlation_id;
        }
    }
    pthread_mutex_unlock( &_request_generation_lock );
    return current;
}

static BOOL _thumbDVResponseIsCurrent( thumbdv_request_generation_queue * queue ) {
    return _thumbDVConsumeResponse( queue, NULL );
}

static uint32 _thumbDVCurrentRequestCount(
    thumbdv_request_generation_queue * queue ) {
    uint32 current_count = 0U;
    pthread_mutex_lock( &_request_generation_lock );
    for ( uint32 i = 0U; i < queue->count; i++ ) {
        const uint32 index = ( queue->head + i )
            % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
        if ( queue->values[index].generation == queue->current ) {
            current_count++;
        }
    }
    pthread_mutex_unlock( &_request_generation_lock );
    return current_count;
}

static void _thumbDVResetRequestGenerations( void ) {
    pthread_mutex_lock( &_request_generation_lock );
    const uint32 encoded_generation = _encoded_requests.current + 1U;
    const uint32 decoded_generation = _decoded_requests.current + 1U;
    memset( &_encoded_requests, 0, sizeof( _encoded_requests ) );
    memset( &_decoded_requests, 0, sizeof( _decoded_requests ) );
    _encoded_requests.current = encoded_generation;
    _decoded_requests.current = decoded_generation;
    pthread_mutex_unlock( &_request_generation_lock );
}

static void _thumbDVPurgeList( BufferDescriptor root,
                               pthread_rwlock_t * lock,
                               uint32 * count,
                               BOOL * buffering ) {
    BufferDescriptor first = NULL;

    pthread_rwlock_wrlock( lock );
    if ( root != NULL && root->next != NULL && root->next != root ) {
        first = root->next;
        BufferDescriptor last = root->prev;
        first->prev = NULL;
        last->next = NULL;
        root->next = root;
        root->prev = root;
    }
    *count = 0U;
    *buffering = TRUE;
    pthread_rwlock_unlock( lock );

    while ( first != NULL ) {
        BufferDescriptor next = first->next;
        first->next = NULL;
        first->prev = NULL;
        hal_BufferRelease( &first );
        first = next;
    }
}

static void _thumbDVListLinkTailLocked( BufferDescriptor root,
                                        BufferDescriptor desc,
                                        uint32 * count,
                                        BOOL * buffering,
                                        uint32 buffering_target,
                                        BOOL log_buffering ) {
    desc->next = root;
    desc->prev = root->prev;
    root->prev->next = desc;
    root->prev = desc;
    ( *count )++;

    if ( *count > buffering_target ) {
        if ( log_buffering && *buffering ) {
            output( "Encoded Buffering is now FALSE\n" );
        }
        *buffering = FALSE;
    }
}

static BOOL _thumbDVQueueResponseIfCurrent(
    thumbdv_request_generation_queue * queue,
    BufferDescriptor root,
    pthread_rwlock_t * list_lock,
    uint32 * count,
    BOOL * buffering,
    uint32 buffering_target,
    BOOL log_buffering,
    const unsigned char * data,
    uint32 length ) {
    BufferDescriptor desc = hal_BufferRequest( length, sizeof( unsigned char ) );
    if ( desc == NULL ) {
        _thumbDVConsumeResponse( queue, NULL );
        return FALSE;
    }
    memcpy( desc->buf_ptr, data, length );

    BOOL current = FALSE;
    uint64 correlation_id = 0U;
    pthread_mutex_lock( &_request_generation_lock );
    if ( queue->count > 0U ) {
        const thumbdv_request request = queue->values[queue->head];
        queue->head = ( queue->head + 1U ) % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
        queue->count--;
        current = request.generation == queue->current;
        correlation_id = request.correlation_id;
    }

    if ( current ) {
        desc->correlation_id = correlation_id;
        pthread_rwlock_wrlock( list_lock );
        _thumbDVListLinkTailLocked( root,
                                    desc,
                                    count,
                                    buffering,
                                    buffering_target,
                                    log_buffering );
        pthread_rwlock_unlock( list_lock );
    }
    pthread_mutex_unlock( &_request_generation_lock );

    if ( !current ) {
        hal_BufferRelease( &desc );
    }
    return current;
}

static BufferDescriptor _thumbDVEncodedList_UnlinkHead(void ) {
    BufferDescriptor buf_desc = NULL;
    pthread_rwlock_wrlock( &_encoded_list_lock );


    if ( _encoded_root == NULL || _encoded_root->next == NULL ) {
        output( "Attempt to unlink from a NULL head" );
        pthread_rwlock_unlock( &_encoded_list_lock );
        return NULL;
    }

    if ( _encoded_buffering ) {
        pthread_rwlock_unlock( &_encoded_list_lock );
        return NULL;
    }

    if ( _encoded_root->next != _encoded_root )
        buf_desc = _encoded_root->next;

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

            if ( _encoded_count > 0 ) _encoded_count--;
        }
    } else {
        if ( !_encoded_buffering ) output( "Encoded list now buffering\n" );

        _encoded_buffering = TRUE;
    }

    pthread_rwlock_unlock( &_encoded_list_lock );
    return buf_desc;
}

static void _thumbDVEncodedList_LinkTail( BufferDescriptor buf_desc ) {
    pthread_rwlock_wrlock( &_encoded_list_lock );
    _thumbDVListLinkTailLocked( _encoded_root,
                                buf_desc,
                                &_encoded_count,
                                &_encoded_buffering,
                                _encode_buffering_target,
                                TRUE );
    pthread_rwlock_unlock( &_encoded_list_lock );
}

static BufferDescriptor _thumbDVDecodedList_UnlinkHead( void ) {
    BufferDescriptor buf_desc = NULL;
    pthread_rwlock_wrlock( &_decoded_list_lock );

    if ( _decoded_root == NULL || _decoded_root->next == NULL ) {
        output( "Attempt to unlink from a NULL head" );
        pthread_rwlock_unlock( &_decoded_list_lock );
        return NULL;
    }

    if ( _decoded_buffering ) {
        pthread_rwlock_unlock( &_decoded_list_lock );
        return NULL;
    }

    if ( _decoded_root->next != _decoded_root ) {
        buf_desc = _decoded_root->next;
    }

    if ( buf_desc != NULL ) {
        //output("0");
        // make sure buffer exists and is actually linked
        if ( !buf_desc || !buf_desc->prev || !buf_desc->next ) {
            output( "Invalid buffer descriptor" );
            buf_desc = NULL;
        } else {
            buf_desc->next->prev = buf_desc->prev;
            buf_desc->prev->next = buf_desc->next;
            buf_desc->next = NULL;
            buf_desc->prev = NULL;

            if ( _decoded_count > 0 ) _decoded_count--;
        }
    } else {
        if ( !_decoded_buffering )
            //output( "DecodedList now Buffering \n" );

        _decoded_buffering = TRUE;
    }

    pthread_rwlock_unlock( &_decoded_list_lock );
    return buf_desc;
}

static void _thumbDVDecodedList_LinkTail( BufferDescriptor buf_desc ) {
    pthread_rwlock_wrlock( &_decoded_list_lock );
    _thumbDVListLinkTailLocked( _decoded_root,
                                buf_desc,
                                &_decoded_count,
                                &_decoded_buffering,
                                _buffering_target,
                                FALSE );
    pthread_rwlock_unlock( &_decoded_list_lock );
}

BOOL thumbDV_getDecodeListBuffering(void)
{
    return _decoded_buffering;
}

static void delay( unsigned int delay ) {
    struct timespec tim, tim2;
    tim.tv_sec = 0;
    tim.tv_nsec = delay * 1000UL;
    nanosleep( &tim, &tim2 );
};

void thumbDV_flushLists(void)
{
    pthread_mutex_lock( &_request_generation_lock );
    _encoded_requests.current++;
    _decoded_requests.current++;
    _thumbDVPurgeList( _encoded_root,
                       &_encoded_list_lock,
                       &_encoded_count,
                       &_encoded_buffering );
    _thumbDVPurgeList( _decoded_root,
                       &_decoded_list_lock,
                       &_decoded_count,
                       &_decoded_buffering );
    pthread_mutex_unlock( &_request_generation_lock );
}

void thumbDV_dump( char * text, unsigned char * data, unsigned int length ) {
    unsigned int offset = 0U;
    unsigned int i;

    output( "%s", text );
    output( "\n" );

    while ( length > 0U ) {
        unsigned int bytes = ( length > 16U ) ? 16U : length;

        output( "%04X:  ", offset );

        for ( i = 0U; i < bytes; i++ )
            output( "%02X ", data[offset + i] );

        for ( i = bytes; i < 16U; i++ )
            output( "   " );

        output( "   *" );

        for ( i = 0U; i < bytes; i++ ) {
            unsigned char c = data[offset + i];

            if ( isprint( c ) )
                output( "%c", c );
            else
                output( "." );
        }

        output( "*\n" );

        offset += 16U;

        if ( length >= 16U )
            length -= 16U;
        else
            length = 0U;
    }
}

static int thumbDV_writeSerial( FT_HANDLE handle , unsigned char * buffer, uint32 bytes )
{
    FT_STATUS status = FT_OK;
    DWORD written = 0;

    if ( handle != NULL )
    {
		//FT_SetRts(handle);
        status = FT_Write(handle, buffer, bytes, &written);

        if ( status != FT_OK || written != bytes ) {
            const FT_STATUS effective_status = status != FT_OK ? status : FT_IO_ERROR;
            output( ANSI_RED "Could not write to serial port. status = %d\n",
                    effective_status );
            if ( status == FT_OK ) {
                _thumbDVSetLastError(
                    "ThumbDV serial write was incomplete (%lu of %lu bytes)",
                    (unsigned long)written, (unsigned long)bytes );
            } else {
                _thumbDVSetLastError( "ThumbDV serial write failed (status %lu): %s",
                                      (unsigned long)effective_status,
                                      AetherSerial_GetLastError() );
            }
            return effective_status;
        }
        //FT_ClrRts(handle);
    }
    else
    {
        output( ANSI_RED "Could not write to serial port. Timeout\n" ANSI_WHITE );
        _thumbDVSetLastError( "ThumbDV serial handle is unavailable" );
        return FT_INVALID_HANDLE;
    }

    return status;
}

static int _check_serial( FT_HANDLE handle )
{
	int ret  = 0;
    unsigned char reset[5] = { 0x61, 0x00, 0x01, 0x00, 0x33 };

    _last_control_field = 0xffU;
    if ( thumbDV_writeSerial( handle, reset, 5 ) != FT_OK ) {
        return -1;
    }
    ret = thumbDV_processSerial(handle);

    if ( ret != 0 || _last_control_field != 0x39U )
    {
        output( "Could not reset serial port FD = %p \n", (void*)handle );
        _thumbDVSetLastError( "No valid DV3000 reset response was received" );
        return -1;
    }

    unsigned char get_prodID[5] = {0x61, 0x00, 0x01, 0x00, 0x30 };
    _last_control_field = 0xffU;
    if ( thumbDV_writeSerial( handle, get_prodID, 5 ) != FT_OK ) {
        return -1;
    }
    ret = thumbDV_processSerial(handle);

    if ( ret != 0 || _last_control_field != 0x30U )
    {
        output( "Could not read the DV3000 product ID from FD = %p \n", (void*)handle );
        _thumbDVSetLastError( "No valid DV3000 product-ID response was received" );
        return -1;
    }

    return 0 ;
}

static BOOL _configure_serial( FT_HANDLE handle, ULONG baud, USHORT flow_control )
{
    FT_STATUS status = FT_SetBaudRate( handle, baud );
    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to set ThumbDV baud rate %lu (status %lu): %s",
                              (unsigned long)baud, (unsigned long)status,
                              AetherSerial_GetLastError() );
        return FALSE;
    }
    status = FT_SetDataCharacteristics( handle, FT_BITS_8, FT_STOP_BITS_1,
                                        FT_PARITY_NONE );
    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to configure ThumbDV framing (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return FALSE;
    }
    status = FT_SetTimeouts( handle, 0, 0 );
    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to configure ThumbDV timeouts (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return FALSE;
    }
    status = FT_SetFlowControl( handle, flow_control, 0, 0 );
    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to configure ThumbDV flow control (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return FALSE;
    }

    status = FT_SetLatencyTimer( handle, 5U );
    if ( status == FT_NOT_SUPPORTED ) {
        if ( !_latency_timer_notice_emitted ) {
            output( "ThumbDV: the OS serial API does not expose the FTDI latency timer; continuing.\n" );
            _latency_timer_notice_emitted = TRUE;
        }
        AetherSerial_ClearLastError();
    } else if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to configure ThumbDV latency (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return FALSE;
    }
    return TRUE;
}

FT_HANDLE thumbDV_openSerial( FT_DEVICE_LIST_INFO_NODE device )
{
    //struct termios tty;
    FT_HANDLE handle = NULL;
    FT_STATUS status = FT_OK;
    output("Trying to open serial port %s \n", device.SerialNumber);

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber[0] != '\0' )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        _thumbDVSetLastError( "Unable to open ThumbDV device %s (status %lu): %s",
                              device.SerialNumber,
                              (unsigned long)status,
                              AetherSerial_GetLastError() );

        return NULL;
    }

    if ( !_configure_serial( handle, FT_BAUD_460800, FT_FLOW_RTS_CTS ) ) {
        output( "Could not configure ThumbDV at 460800 Baud. Trying 230400\n" );
        FT_Close( handle );
        handle = NULL;
    }

/*
    tty.c_cflag = ( tty.c_cflag & ~CSIZE ) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;

    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 5;

    tty.c_iflag &= ~( IXON | IXOFF | IXANY );

    tty.c_cflag |= ( CLOCAL | CREAD );

    tty.c_cflag &= ~( PARENB | PARODD );
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if ( tcsetattr( fd, TCSANOW, &tty ) != 0 ) {
        output( "ThumbDV: error %d from tcsetattr\n", errno );
        close( fd );
        return -1;
    }
*/
    if ( handle != NULL ) {
        if ( _check_serial(handle) == 0 ) {
            return handle;
        }
        output( "Could not detect ThumbDV at 460800 Baud. Trying 230400\n" );
        FT_Close(handle);
        handle = (FT_HANDLE)NULL;
    }

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber[0] != '\0' )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        _thumbDVSetLastError( "Unable to reopen ThumbDV device %s (status %lu): %s",
                              device.SerialNumber,
                              (unsigned long)status,
                              AetherSerial_GetLastError() );

        return NULL;
    }

    if ( !_configure_serial( handle, FT_BAUD_230400, FT_FLOW_NONE ) ) {
        output( "Could not configure ThumbDV at 230400 Baud\n" );
        FT_Close( handle );
        return NULL;
    }

    if ( _check_serial( handle ) != 0 ) {
        output( "Could not detect ThumbDV at 230400 Baud\n" );
        FT_Close(handle);
        handle = NULL;
        return NULL;
    }

    return handle;
}

static BOOL _openSerialOnce( FT_HANDLE * handle )
{
    DWORD numDevs = 0;
    FT_DEVICE_LIST_INFO_NODE * devInfo = NULL;
    FT_STATUS status = FT_CreateDeviceInfoList( &numDevs );
    *handle = NULL;

    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to enumerate ThumbDV serial devices (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return FALSE;
    }
    if ( numDevs == 0U ) {
        _thumbDVSetLastError( "The configured ThumbDV serial device was not found" );
        return FALSE;
    }

    devInfo = (FT_DEVICE_LIST_INFO_NODE *)safe_malloc(
        sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs );
    status = FT_GetDeviceInfoList( devInfo, &numDevs );
    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to read ThumbDV serial device information (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        safe_free( devInfo );
        return FALSE;
    }

    for ( DWORD i = 0U; i < numDevs; i++ ) {
        *handle = thumbDV_openSerial( devInfo[i] );
        if ( *handle != NULL ) {
            break;
        }
    }
    safe_free( devInfo );
    return *handle != NULL;
}

int thumbDV_probeConfiguredSerial( void )
{
    FT_HANDLE handle = NULL;
    if ( !_openSerialOnce( &handle ) ) {
        return -1;
    }
    const FT_STATUS closeStatus = FT_Close( handle );
    if ( closeStatus != FT_OK ) {
        _thumbDVSetLastError( "ThumbDV probe succeeded but closing the serial device failed: %s",
                              AetherSerial_GetLastError() );
        return -1;
    }
    return 0;
}

int thumbDV_processSerial( FT_HANDLE handle )
{
    unsigned char buffer[BUFFER_LENGTH] = {0};
    unsigned int respLen;

    unsigned char packet_type;
    FT_STATUS status = FT_OK;
    DWORD rx_bytes = 0;
    DWORD tx_bytes = 0 ;
    DWORD event_word = 0;
    uint32 max_us_sleep = 100000; // 100 ms
    uint32 us_slept = 0;
#ifdef _WIN32
    const uint32 poll_sleep_us = 1000U;
#else
    const uint32 poll_sleep_us = 100U;
#endif
    do
    {
        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);

        if ( rx_bytes >= AMBE3000_HEADER_LEN )
            break;

        usleep(poll_sleep_us);

        us_slept += poll_sleep_us;

        if ( us_slept > max_us_sleep )
        {
            output("TimeOut #1\n");
            _thumbDVSetLastError( "Timed out waiting for a DV3000 packet header" );
            return FT_OTHER_ERROR;
        }

    } while (rx_bytes < AMBE3000_HEADER_LEN && status == FT_OK );

    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to query the ThumbDV receive queue (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return status;
    }

    status = FT_Read(handle, buffer, AMBE3000_HEADER_LEN, &rx_bytes);

    if ( status != FT_OK || rx_bytes != AMBE3000_HEADER_LEN)
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
        if ( status == FT_OK ) {
            _thumbDVSetLastError(
                "DV3000 packet header was incomplete (%lu of %u bytes)",
                (unsigned long)rx_bytes, AMBE3000_HEADER_LEN );
        } else {
            _thumbDVSetLastError( "Unable to read a DV3000 packet header (status %lu): %s",
                                  (unsigned long)status, AetherSerial_GetLastError() );
        }
        return 1;
    }

    if ( buffer[0U] != AMBE3000_START_BYTE ) {
        output( ANSI_RED "ThumbDV: unknown byte from the DV3000, 0x%02X\n" ANSI_WHITE, buffer[0U] );
        _thumbDVSetLastError( "DV3000 returned an invalid packet start byte 0x%02X",
                              buffer[0U] );
        return FT_OTHER_ERROR;
    }

    respLen = buffer[1U] * 256U + buffer[2U];
    if ( respLen > BUFFER_LENGTH - AMBE3000_HEADER_LEN ) {
        output( ANSI_RED "ThumbDV: serial packet too large, len=%u max=%u\n" ANSI_WHITE,
                respLen, BUFFER_LENGTH - AMBE3000_HEADER_LEN );
        _thumbDVSetLastError( "DV3000 returned an oversized packet (%u bytes)", respLen );
        return FT_OTHER_ERROR;
    }

    us_slept = 0;
    do
    {
        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);

        if ( rx_bytes >= respLen )
            break;

        usleep(poll_sleep_us);

        us_slept += poll_sleep_us ;

        if ( us_slept > max_us_sleep )
        {
            output("TimeOut #2 \n");
            _thumbDVSetLastError( "Timed out waiting for a complete DV3000 packet" );
            return FT_OTHER_ERROR;
        }

    } while (rx_bytes < respLen && status == FT_OK);

    if ( status != FT_OK ) {
        _thumbDVSetLastError( "Unable to query the ThumbDV receive queue (status %lu): %s",
                              (unsigned long)status, AetherSerial_GetLastError() );
        return status;
    }

    status = FT_Read(handle, buffer + AMBE3000_HEADER_LEN , respLen, &rx_bytes);

    if ( status != FT_OK || rx_bytes != respLen )
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
        if ( status == FT_OK ) {
            _thumbDVSetLastError(
                "DV3000 packet payload was incomplete (%lu of %u bytes)",
                (unsigned long)rx_bytes, respLen );
        } else {
            _thumbDVSetLastError( "Unable to read a DV3000 packet payload (status %lu): %s",
                                  (unsigned long)status, AetherSerial_GetLastError() );
        }
        return FT_OTHER_ERROR;
    }

    respLen += AMBE3000_HEADER_LEN;

    packet_type = buffer[3];

    //thumbDV_dump("Serial data", buffer, respLen);
    if ( packet_type == AMBE3000_CTRL_PKT_TYPE ) {
        if ( respLen >= AMBE3000_HEADER_LEN + 1U ) {
            _last_control_field = buffer[AMBE3000_HEADER_LEN];
        }
        thumbDV_dump( ANSI_YELLOW "Serial data" ANSI_WHITE, buffer, respLen );
    } else if ( packet_type == AMBE3000_CHAN_PKT_TYPE ) {
        if ( !_thumbDVQueueResponseIfCurrent( &_encoded_requests,
                                              _encoded_root,
                                              &_encoded_list_lock,
                                              &_encoded_count,
                                              &_encoded_buffering,
                                              _encode_buffering_target,
                                              TRUE,
                                              buffer,
                                              respLen ) ) {
            output( "AETHER_DSTAR_DIAG stale_thumbdv_response type=encoded\n" );
        }
    } else if ( packet_type == AMBE3000_SPEECH_PKT_TYPE ) {
        if ( !_thumbDVQueueResponseIfCurrent( &_decoded_requests,
                                              _decoded_root,
                                              &_decoded_list_lock,
                                              &_decoded_count,
                                              &_decoded_buffering,
                                              _buffering_target,
                                              FALSE,
                                              buffer,
                                              respLen ) ) {
            output( "AETHER_DSTAR_DIAG stale_thumbdv_response type=decoded\n" );
        }
    } else {
        output( ANSI_RED "Unrecognized packet type 0x%02X ", packet_type );
        _thumbDVSetLastError( "DV3000 returned unsupported packet type 0x%02X",
                              packet_type );
        return FT_OTHER_ERROR;

    }


    return FT_OK;
}

int thumbDV_unlinkAudio(short * speech_out)
{
    int32 samples_returned = 0;
    if ( speech_out == NULL ) {
        return samples_returned;
    }

    BufferDescriptor desc = _thumbDVDecodedList_UnlinkHead();
    uint32 samples_in_speech_packet = 0;
    uint32 length = 0;

    if ( desc != NULL ) {
        if ( desc->num_samples < 6U ) {
            output( ANSI_YELLOW "ThumbDV: dropping short speech packet, len=%u\n" ANSI_WHITE,
                    desc->num_samples );
            hal_BufferRelease(&desc);
            return samples_returned;
        }

        length = ( ( ( unsigned char * )desc->buf_ptr )[1] << 8 ) + ( ( unsigned char * )desc->buf_ptr )[2];;

        if ( length != 0x142 ) {
            output( ANSI_YELLOW "WARNING LENGTH DOESN'T MATCH %u " ANSI_WHITE, length );
            thumbDV_dump( "MISMATHCED", ( ( unsigned char * ) desc->buf_ptr ), desc->num_samples );
        }

        samples_in_speech_packet = ( ( unsigned char * )desc->buf_ptr )[5];
        uint32 max_samples_in_packet = ( desc->num_samples - 6U ) / 2U;
        if ( samples_in_speech_packet > max_samples_in_packet ) {
            output( ANSI_YELLOW "ThumbDV: clipping speech packet samples from %u to %u\n" ANSI_WHITE,
                    samples_in_speech_packet, max_samples_in_packet );
            samples_in_speech_packet = max_samples_in_packet;
        }
        if ( samples_in_speech_packet > THUMBDV_DSTAR_PCM_SAMPLES ) {
            output( ANSI_YELLOW "ThumbDV: clipping speech output samples from %u to %u\n" ANSI_WHITE,
                    samples_in_speech_packet, THUMBDV_DSTAR_PCM_SAMPLES );
            samples_in_speech_packet = THUMBDV_DSTAR_PCM_SAMPLES;
        }

        unsigned char * idx = &( ( ( unsigned char * )desc->buf_ptr )[6] );
        uint32 i = 0;

        for ( i = 0; i < samples_in_speech_packet; i++, idx += 2 ) {
            speech_out[i] = ( idx[0] << 8 ) + idx[1];
        }

        samples_returned = samples_in_speech_packet;

        if ( samples_returned != 160 ) output( "Rate Mismatch expected %d got %d\n", 160, samples_returned );

//        safe_free( desc );
        hal_BufferRelease(&desc);
    } else {
        /* Do nothing for now */
    }

    return samples_returned;
}

void thumbDV_decode( FT_HANDLE handle, unsigned char * packet_in, uint8 bytes_in_packet ) {
    uint32 i = 0;

    unsigned char full_packet[15] = {0};

    if ( packet_in != NULL && bytes_in_packet >= THUMBDV_DSTAR_AMBE_BYTES && handle != NULL ) {
        full_packet[0] = 0x61;
        full_packet[1] = 0x00;
        full_packet[2] = 0x0B;
        full_packet[3] = 0x01;
        full_packet[4] = 0x01;
        full_packet[5] = 0x48;
        uint32 j = 0;

        for ( i = 0, j = 8  ; i < 9 ; i++ , j-- ) {
            full_packet[i + 6] = packet_in[i];
        }

//        thumbDV_dump("Just AMBE", packet_in, 9);
//        thumbDV_dump("Encoded packet:", full_packet, 15);
        if ( thumbDV_writeSerial( handle, full_packet, 15 ) == FT_OK ) {
            _thumbDVRequestGenerationEnqueue( &_decoded_requests, 0U );
            sem_post(&_read_sem);
        }
    }
}

BOOL thumbDV_submitEncode( FT_HANDLE handle,
                           const short * speech_in,
                           uint8 num_of_samples,
                           uint64 correlation_id )
{
    if ( speech_in == NULL || num_of_samples == 0U || handle == NULL ) {
        return FALSE;
    }

    unsigned char packet[THUMBDV_MAX_PACKET_LEN];
    uint16 speech_d_bytes = num_of_samples * sizeof( uint16 ); /* Should be 2 times the number of samples */

    /* Calculate length of packet NOT including the full header just the type field*/
    uint16 length = 0;
    /* Includes Channel Field and SpeechD Field Header */
    length += AMBE3000_SPEECHD_HEADER_LEN;
    length += speech_d_bytes;

    /* Will be used to write fields into packet */
    unsigned char * idx = &packet[0];

    *( idx++ ) = AMBE3000_START_BYTE;
    /* Length split into two bytes */
    *( idx++ ) = length >> 8;
    *( idx++ ) = length & 0xFF;
    /* SPEECHD Type */
    *( idx++ ) = AMBE3000_SPEECH_PKT_TYPE;
    /* Channel0 Identifier */
    *( idx++ ) = 0x40;
    /* SPEEECHD Identifier */
    *( idx++ ) = 0x00;
    /* SPEECHD No of Samples */
    *( idx++ ) = num_of_samples;
    uint32 i = 0;
//    output("Num of Samples = 0x%X\n", num_of_samples);

#ifdef WOOT
    output( "Encode Packet Header = " );
    unsigned char * p = &packet[0];
    i = 0;

    for ( i = 0 ; i < 7 ; i++ ) {

        output( "%02X ", *p );
        p++;
    }

    output( "\n" );

#endif
    //memcpy(idx, speech_in, speech_d_bytes);
    i = 0;

    for ( i = 0 ; i < num_of_samples ; i++, idx += 2 ) {
        idx[0] = speech_in[i] >> 8;
        idx[1] = ( speech_in[i] & 0x00FF ) ;
    }

    if ( thumbDV_writeSerial( handle, packet,
                             length + AMBE3000_HEADER_LEN ) != FT_OK ) {
        return FALSE;
    }
    _thumbDVRequestGenerationEnqueue( &_encoded_requests, correlation_id );
    sem_post(&_read_sem);
    return TRUE;
}

int thumbDV_takeEncoded( unsigned char * packet_out,
                         uint8 packet_capacity,
                         uint64 * correlation_id )
{
    if ( packet_out == NULL || packet_capacity == 0U ) {
        return 0;
    }
    int32 samples_returned = 0;
    BufferDescriptor desc = _thumbDVEncodedList_UnlinkHead();

    if ( desc != NULL ) {
        if ( desc->num_samples > 6U ) {
            uint32 encoded_bytes = desc->sample_size * ( desc->num_samples - 6U );
            if ( encoded_bytes > packet_capacity ) {
                output( ANSI_YELLOW "ThumbDV: clipping encoded AMBE bytes from %u to %u\n" ANSI_WHITE,
                        encoded_bytes, packet_capacity );
                encoded_bytes = packet_capacity;
            }
            if ( encoded_bytes > THUMBDV_DSTAR_AMBE_BYTES ) {
                output( ANSI_YELLOW "ThumbDV: clipping encoded D-Star AMBE bytes from %u to %u\n" ANSI_WHITE,
                        encoded_bytes, THUMBDV_DSTAR_AMBE_BYTES );
                encoded_bytes = THUMBDV_DSTAR_AMBE_BYTES;
            }
            memcpy( packet_out, ( ( const unsigned char * )desc->buf_ptr ) + 6,
                    encoded_bytes );
            samples_returned = encoded_bytes;
            if ( correlation_id != NULL ) {
                *correlation_id = desc->correlation_id;
            }
        } else {
            output( ANSI_YELLOW "ThumbDV: dropping short encoded packet, len=%u\n" ANSI_WHITE,
                    desc->num_samples );
        }
        //safe_free( desc );
        hal_BufferRelease(&desc);
        //thumbDV_dump(ANSI_BLUE "Coded Packet" ANSI_WHITE, packet_out, desc->num_samples - 6);

    } else {
        /* Do nothing for now */
    }

    return samples_returned;
}

uint32 thumbDV_pendingEncodeRequests( void )
{
    return _thumbDVCurrentRequestCount( &_encoded_requests );
}

uint32 thumbDV_availableEncodedResponses( void )
{
    pthread_rwlock_rdlock( &_encoded_list_lock );
    const uint32 count = _encoded_count;
    pthread_rwlock_unlock( &_encoded_list_lock );
    return count;
}

uint32 thumbDV_encodeOutstanding( void )
{
    uint32 pending = 0U;
    pthread_mutex_lock( &_request_generation_lock );
    for ( uint32 i = 0U; i < _encoded_requests.count; i++ ) {
        const uint32 index = ( _encoded_requests.head + i )
            % THUMBDV_REQUEST_GENERATION_QUEUE_LENGTH;
        if ( _encoded_requests.values[index].generation
             == _encoded_requests.current ) {
            pending++;
        }
    }
    pthread_rwlock_rdlock( &_encoded_list_lock );
    const uint32 available = _encoded_count;
    pthread_rwlock_unlock( &_encoded_list_lock );
    pthread_mutex_unlock( &_request_generation_lock );
    return UINT32_MAX - pending < available
        ? UINT32_MAX
        : pending + available;
}

int thumbDV_encode( FT_HANDLE handle, short * speech_in,
                    unsigned char * packet_out, uint8 num_of_samples )
{
    if ( speech_in == NULL || packet_out == NULL || num_of_samples == 0U ) {
        return 0;
    }
    ( void )thumbDV_submitEncode( handle, speech_in, num_of_samples, 0U );
    return thumbDV_takeEncoded( packet_out, num_of_samples, NULL );

}

static BOOL _initializeSerial( FT_HANDLE handle )
{
    unsigned char disable_parity[6] = {0x61, 0x00, 0x02, 0x00, 0x3F, 0x00};
    unsigned char get_version[5] = {0x61, 0x00, 0x01, 0x00, 0x31};
    unsigned char read_cfg[5] = {0x61, 0x00, 0x01, 0x00, 0x37};
    unsigned char dstar_mode[17] = {0x61, 0x00, 0x0D, 0x00, 0x0A, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};
    unsigned char pkt_init[6] = { 0x61, 0x00, 0x02, 0x00, 0x0B, 0x07 };
    unsigned char pkt_gain[7] = { 0x61, 0x00, 0x03, 0x00, 0x4B, 0x00, 0x00 };
    unsigned char pkt_compand[6] = { 0x61, 0x00, 0x02, 0x00, 0x32, 0x00 };
    unsigned char pkt_fmt[7] = {0x61, 0x00, 0x3, 0x00, 0x15, 0x00, 0x00};

    unsigned char * commands[] = {
        disable_parity, get_version, read_cfg, dstar_mode,
        pkt_init, pkt_gain, pkt_compand, pkt_fmt
    };
    const uint32 lengths[] = { 6U, 5U, 5U, 17U, 6U, 7U, 6U, 7U };
    for ( uint32 i = 0U; i < sizeof( lengths ) / sizeof( lengths[0] ); i++ ) {
        if ( thumbDV_writeSerial( handle, commands[i], lengths[i] ) != FT_OK
             || thumbDV_processSerial( handle ) != FT_OK ) {
            char detail[sizeof( _thumbDV_last_error )];
            snprintf( detail, sizeof( detail ), "%s", thumbDV_lastError() );
            _thumbDVSetLastError( "ThumbDV initialization command %lu failed: %s",
                                  (unsigned long)i, detail );
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL _failFastPolicy( BOOL initial_connection, const char * value )
{
    return initial_connection && value != NULL && strcmp( value, "0" ) != 0;
}

static BOOL _drainReadSemaphore( void )
{
    for ( ;; ) {
        if ( sem_trywait( &_read_sem ) == 0 ) {
            continue;
        }
        if ( errno == EINTR ) {
            continue;
        }
        if ( errno == EAGAIN ) {
            return TRUE;
        }
        _thumbDVSetLastError( "Unable to drain the ThumbDV read queue: %s",
                              strerror( errno ) );
        return FALSE;
    }
}

static void _connectSerial( FT_HANDLE * ftHandle, BOOL initial_connection )
{
    output("ConnectSerial\n");

    do {
        if ( _openSerialOnce( ftHandle ) && _initializeSerial( *ftHandle ) ) {
            output( "AETHER_DV_DEVICE state=connected\n" );
            return;
        }

        if ( *ftHandle != NULL ) {
            FT_Close( *ftHandle );
            *ftHandle = NULL;
        }

        const char * fail_fast = getenv("AETHER_DV_FAIL_FAST");
        if ( _failFastPolicy( initial_connection, fail_fast ) ) {
            output( "AETHER_DV_ERROR %s\n", thumbDV_lastError() );
            output( "Could not initialize ThumbDV serial device; exiting because fail-fast is enabled.\n" );
            exit(3);
        }
        output( "AETHER_DV_DEVICE state=waiting detail=%s\n", thumbDV_lastError() );
        usleep( 1000 * 1000 );
    } while ( *ftHandle == NULL );
}

static void * _thumbDV_readThread( void * param )
{
    FT_HANDLE handle = *(FT_HANDLE *) param;

    prctl(PR_SET_NAME, "DV-Read");

    while ( !_readThreadAbort )
    {
        sem_wait(&_read_sem);

        if (!_thumbDVReadAllowed())
        {
            break;
        }
        else
        {
            const int result = thumbDV_processSerial(handle);
            if ( result != FT_OK ) {
                _thumbDVReportSerialFault( thumbDV_lastError() );
                output( ANSI_RED
                        "ThumbDV read parser failed; scheduling reconnect: %s\n"
                        ANSI_WHITE,
                        thumbDV_lastError() );
                break;
            }
        }
    }
    output( ANSI_YELLOW "thumbDV_readThread has exited\n" ANSI_WHITE );
    return 0;
}

static void * _thumbDV_connectThread( void * param )
{
    int ret;
    DWORD rx_bytes;
    DWORD tx_bytes;
    DWORD event_dword;

    FT_HANDLE * sharedHandle = (FT_HANDLE *)param;
    FT_HANDLE handle = *sharedHandle;

    while ( !_connectThreadAbort ) {

        //waits 1 second before checking status to prevent CPU hogging
        usleep(1000000);
        char parserFault[512] = {0};
        const BOOL parserFailed = _thumbDVTakeSerialFault(
            parserFault, sizeof( parserFault ) );
        ret = parserFailed
            ? FT_IO_ERROR
            : FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_dword);

        if (ret != FT_OK) {

            FT_HANDLE staleHandle = handle;
            char disconnectReason[512];
            snprintf( disconnectReason, sizeof( disconnectReason ), "%s",
                      parserFailed
                          ? parserFault
                          : AetherSerial_GetLastError() );

            // Stop new scheduler writes before closing the stale OS handle.
            handle = NULL;
            *sharedHandle = NULL;
            sched_waveform_setHandle(&handle);

            //clear out read buffer and stop read thread
            _thumbDVSetReadAllowed( FALSE );
            sem_post(&_read_sem);
            pthread_join( _read_thread, NULL );

            thumbDV_flushLists();
            if ( !_drainReadSemaphore() ) {
                output( "AETHER_DV_ERROR %s\n", thumbDV_lastError() );
                exit(3);
            }

            if ( staleHandle != NULL ) {
                FT_Close( staleHandle );
            }

            output("AETHER_DV_DEVICE state=disconnected detail=%s\n",
                   disconnectReason[0] != '\0'
                       ? disconnectReason
                       : "serial device no longer responds");

            // Runtime disconnects keep retrying even when startup fail-fast is enabled.
            _connectSerial(&handle, FALSE);
            _thumbDVResetRequestGenerations();
            *sharedHandle = handle;
            sched_waveform_setHandle(&handle);

            //Start read thread again
            _thumbDVSetReadAllowed( TRUE );
            pthread_create( &_read_thread, NULL, &_thumbDV_readThread, sharedHandle );
        }
    }
    return NULL;
}

void thumbDV_init( FT_HANDLE * handle ) {
    pthread_rwlock_init( &_encoded_list_lock, NULL );
    pthread_rwlock_init( &_decoded_list_lock, NULL );

    sem_init(&_read_sem, 0, 0);

    pthread_mutex_lock( &_request_generation_lock );
    memset( &_encoded_requests, 0, sizeof( _encoded_requests ) );
    memset( &_decoded_requests, 0, sizeof( _decoded_requests ) );
    pthread_mutex_unlock( &_request_generation_lock );
    pthread_mutex_lock( &_serial_fault_lock );
    _serial_fault_pending = FALSE;
    _serial_fault_detail[0] = '\0';
    pthread_mutex_unlock( &_serial_fault_lock );

    pthread_rwlock_wrlock( &_encoded_list_lock );
    _encoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _encoded_root, 0, sizeof( buffer_descriptor ) );
    _encoded_root->next = _encoded_root;
    _encoded_root->prev = _encoded_root;
    pthread_rwlock_unlock( &_encoded_list_lock );

    pthread_rwlock_wrlock( &_decoded_list_lock );
    _decoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _decoded_root, 0, sizeof( buffer_descriptor ) );
    _decoded_root->next = _decoded_root;
    _decoded_root->prev = _decoded_root;
    pthread_rwlock_unlock( &_decoded_list_lock );

    _connectSerial(handle, TRUE);
    sched_waveform_setHandle(handle);

    pthread_create( &_connect_thread, NULL, &_thumbDV_connectThread, handle );
    pthread_create( &_read_thread, NULL, &_thumbDV_readThread, handle );

    struct sched_param fifo_param;
    fifo_param.sched_priority = 30;
    pthread_setschedparam( _read_thread, SCHED_FIFO, &fifo_param );

}

#ifdef AETHER_DSTAR_TESTING
void thumbDV_testReportSerialFault(const char * detail)
{
    _thumbDVReportSerialFault( detail );
}

BOOL thumbDV_testTakeSerialFault(char * detail, uint32 detail_size)
{
    return _thumbDVTakeSerialFault( detail, detail_size );
}

void thumbDV_testInitializeQueues(void)
{
    pthread_rwlock_init( &_encoded_list_lock, NULL );
    pthread_rwlock_init( &_decoded_list_lock, NULL );

    _encoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _encoded_root, 0, sizeof( buffer_descriptor ) );
    _encoded_root->next = _encoded_root;
    _encoded_root->prev = _encoded_root;

    _decoded_root = ( BufferDescriptor )safe_malloc( sizeof( buffer_descriptor ) );
    memset( _decoded_root, 0, sizeof( buffer_descriptor ) );
    _decoded_root->next = _decoded_root;
    _decoded_root->prev = _decoded_root;

    _encoded_count = 0U;
    _decoded_count = 0U;
    _encoded_buffering = TRUE;
    _decoded_buffering = TRUE;
    memset( &_encoded_requests, 0, sizeof( _encoded_requests ) );
    memset( &_decoded_requests, 0, sizeof( _decoded_requests ) );
}

void thumbDV_testQueueEncodedFrames(uint32 count)
{
    uint32 i;
    for ( i = 0U; i < count; i++ ) {
        BufferDescriptor desc = hal_BufferRequest( 1U, 1U );
        _thumbDVEncodedList_LinkTail( desc );
    }
}

void thumbDV_testQueueDecodedFrames(uint32 count)
{
    uint32 i;
    for ( i = 0U; i < count; i++ ) {
        BufferDescriptor desc = hal_BufferRequest( 1U, 1U );
        _thumbDVDecodedList_LinkTail( desc );
    }
}

void thumbDV_testEnqueueEncodedRequest(void)
{
    _thumbDVRequestGenerationEnqueue( &_encoded_requests, 0U );
}

void thumbDV_testEnqueueEncodedRequestWithCorrelation(uint64 correlation_id)
{
    _thumbDVRequestGenerationEnqueue( &_encoded_requests, correlation_id );
}

void thumbDV_testEnqueueDecodedRequest(void)
{
    _thumbDVRequestGenerationEnqueue( &_decoded_requests, 0U );
}

BOOL thumbDV_testConsumeEncodedResponse(void)
{
    return _thumbDVResponseIsCurrent( &_encoded_requests );
}

BOOL thumbDV_testConsumeDecodedResponse(void)
{
    return _thumbDVResponseIsCurrent( &_decoded_requests );
}

BOOL thumbDV_testQueueEncodedResponse(void)
{
    const unsigned char data = 0U;
    return _thumbDVQueueResponseIfCurrent( &_encoded_requests,
                                           _encoded_root,
                                           &_encoded_list_lock,
                                           &_encoded_count,
                                           &_encoded_buffering,
                                           _encode_buffering_target,
                                           FALSE,
                                           &data,
                                           1U );
}

BOOL thumbDV_testQueueDecodedResponse(void)
{
    const unsigned char data = 0U;
    return _thumbDVQueueResponseIfCurrent( &_decoded_requests,
                                           _decoded_root,
                                           &_decoded_list_lock,
                                           &_decoded_count,
                                           &_decoded_buffering,
                                           _buffering_target,
                                           FALSE,
                                           &data,
                                           1U );
}

uint64 thumbDV_testEncodedHeadCorrelation(void)
{
    pthread_rwlock_rdlock( &_encoded_list_lock );
    const uint64 correlation_id = _encoded_root != NULL
        && _encoded_root->next != NULL
        && _encoded_root->next != _encoded_root
        ? _encoded_root->next->correlation_id
        : 0U;
    pthread_rwlock_unlock( &_encoded_list_lock );
    return correlation_id;
}

uint32 thumbDV_testPendingEncodedRequests(void)
{
    return thumbDV_pendingEncodeRequests();
}

uint32 thumbDV_testEncodeOutstanding(void)
{
    return thumbDV_encodeOutstanding();
}

uint32 thumbDV_testEncodedFrameCount(void)
{
    return _encoded_count;
}

uint32 thumbDV_testDecodedFrameCount(void)
{
    return _decoded_count;
}

BOOL thumbDV_testEncodedBuffering(void)
{
    return _encoded_buffering;
}

BOOL thumbDV_testDecodedBuffering(void)
{
    return _decoded_buffering;
}

BOOL thumbDV_testFailFastPolicy(BOOL initial_connection, const char * value)
{
    return _failFastPolicy(initial_connection, value);
}

BOOL thumbDV_testReadSemaphoreReset(void)
{
    if ( sem_init( &_read_sem, 0, 3 ) != 0 ) {
        return FALSE;
    }
    if ( !_drainReadSemaphore() ) {
        return FALSE;
    }
    const BOOL empty = sem_trywait( &_read_sem ) != 0 && errno == EAGAIN;
    sem_destroy( &_read_sem );
    return empty;
}

void thumbDV_testDestroyQueues(void)
{
    thumbDV_flushLists();
    safe_free( _encoded_root );
    safe_free( _decoded_root );
    _encoded_root = NULL;
    _decoded_root = NULL;
    pthread_rwlock_destroy( &_encoded_list_lock );
    pthread_rwlock_destroy( &_decoded_list_lock );
}
#endif
