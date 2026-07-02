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
static uint32 _encode_buffering_target = 4;

static pthread_rwlock_t _encoded_list_lock;
static BufferDescriptor _encoded_root;
static BOOL _encoded_buffering = TRUE;
static uint32 _encoded_count = 0;

static pthread_rwlock_t _decoded_list_lock;
static BufferDescriptor _decoded_root;
static BOOL _decoded_buffering = TRUE;
static uint32 _decoded_count = 0;

static sem_t _read_sem;

//static void * _thumbDV_readThread( void * param );

BOOL allowedToRead = TRUE;

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
    buf_desc->next = _encoded_root;
    buf_desc->prev = _encoded_root->prev;
    _encoded_root->prev->next = buf_desc;
    _encoded_root->prev = buf_desc;
    _encoded_count++;

    if ( _encoded_count > _encode_buffering_target ) {
        if ( _encoded_buffering ) output( "Encoded Buffering is now FALSE\n" );

        _encoded_buffering = FALSE;
    }

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
    buf_desc->next = _decoded_root;
    buf_desc->prev = _decoded_root->prev;
    _decoded_root->prev->next = buf_desc;
    _decoded_root->prev = buf_desc;

    _decoded_count++;

    if ( _decoded_count > _buffering_target ) {
       // if ( _decoded_buffering ) output( "Decoded Buffering is now FALSE\n" );

        _decoded_buffering = FALSE;
    }

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
    BufferDescriptor buf_desc = NULL;

    do
    {
        buf_desc = _thumbDVEncodedList_UnlinkHead();
        if ( buf_desc != NULL )
            hal_BufferRelease(&buf_desc);
    } while (buf_desc != NULL );


    do
    {
        buf_desc = _thumbDVDecodedList_UnlinkHead();
        if ( buf_desc != NULL )
            hal_BufferRelease(&buf_desc);
    } while (buf_desc != NULL );
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
            output( ANSI_RED "Could not write to serial port. status = %d\n", status );
            return status;
        }
        //FT_ClrRts(handle);
    }
    else
    {
        output( ANSI_RED "Could not write to serial port. Timeout\n" ANSI_WHITE );
    }

    return status;
}

static int _check_serial( FT_HANDLE handle )
{
	int ret  = 0;
    unsigned char reset[5] = { 0x61, 0x00, 0x01, 0x00, 0x33 };

    thumbDV_writeSerial( handle, reset, 5 );
    ret = thumbDV_processSerial(handle);

    if ( ret != 0 )
    {
        output( "Could not reset serial port FD = %p \n", (void*)handle );
        return -1;
    }

    unsigned char get_prodID[5] = {0x61, 0x00, 0x01, 0x00, 0x30 };
    thumbDV_writeSerial( handle, get_prodID, 5 );
    ret = thumbDV_processSerial(handle);

    if ( ret != 0 )
    {
        output( "Could not reset serial port FD = %p \n", (void*)handle );
        return -1;
    }

    return 0 ;
}

FT_HANDLE thumbDV_openSerial( FT_DEVICE_LIST_INFO_NODE device )
{
    //struct termios tty;
    FT_HANDLE handle = NULL;
    FT_STATUS status = FT_OK;
    UCHAR latency = 5;

    output("Trying to open serial port %s \n", device.SerialNumber);

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        return NULL;
    }

    FT_SetBaudRate(handle, FT_BAUD_460800);
    FT_SetDataCharacteristics(handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetTimeouts(handle, 0, 0);
    FT_SetFlowControl(handle, FT_FLOW_RTS_CTS, 0, 0);

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
    if ( _check_serial(handle) != 0 ) {
        output( "Could not detect ThumbDV at 460800 Baud. Trying 230400\n" );
        FT_Close(handle);
        handle = (FT_HANDLE)NULL;
    } else {
        return handle;
    }

    status = FT_OpenEx(device.SerialNumber, FT_OPEN_BY_SERIAL_NUMBER, &handle);

    if ( status != FT_OK || handle == NULL )
    {
        if ( device.SerialNumber )
            output("Error opening device %s - error 0x%X\n", device.SerialNumber, status);
        else
            output("Error opening device - error 0x%X\n", status);

        return NULL;
    }

    FT_SetBaudRate(handle, FT_BAUD_230400 );
    FT_SetDataCharacteristics(handle, FT_BITS_8, FT_STOP_BITS_1, FT_PARITY_NONE);
    FT_SetTimeouts(handle, 0, 0);
    FT_SetFlowControl(handle, FT_FLOW_NONE, 0, 0);
    FT_SetLatencyTimer(handle, latency);

    if ( _check_serial( handle ) != 0 ) {
        output( "Could not detect THumbDV at 230400 Baud\n" );
        FT_Close(handle);
        handle = NULL;
        return NULL;
    }

    return handle;
}

int thumbDV_processSerial( FT_HANDLE handle )
{
    unsigned char buffer[BUFFER_LENGTH];
    unsigned int respLen;

    unsigned char packet_type;
    FT_STATUS status = FT_OK;
    DWORD rx_bytes = 0;
    DWORD tx_bytes = 0 ;
    DWORD event_word = 0;
    uint32 max_us_sleep = 100000; // 100 ms
    uint32 us_slept = 0;
    do
    {
        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);

        if ( rx_bytes >= AMBE3000_HEADER_LEN )
            break;

        usleep(100);

        us_slept += 100;

        if ( us_slept > max_us_sleep )
        {
            output("TimeOut #1\n");
            return FT_OTHER_ERROR;
        }

    } while (rx_bytes < AMBE3000_HEADER_LEN && status == FT_OK );

    status = FT_Read(handle, buffer, AMBE3000_HEADER_LEN, &rx_bytes);

    if ( status != FT_OK || rx_bytes != AMBE3000_HEADER_LEN)
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
        return 1;
    }

    if ( buffer[0U] != AMBE3000_START_BYTE ) {
        output( ANSI_RED "ThumbDV: unknown byte from the DV3000, 0x%02X\n" ANSI_WHITE, buffer[0U] );
        return FT_OTHER_ERROR;
    }

    respLen = buffer[1U] * 256U + buffer[2U];
    if ( respLen > BUFFER_LENGTH - AMBE3000_HEADER_LEN ) {
        output( ANSI_RED "ThumbDV: serial packet too large, len=%u max=%u\n" ANSI_WHITE,
                respLen, BUFFER_LENGTH - AMBE3000_HEADER_LEN );
        return FT_OTHER_ERROR;
    }

    us_slept = 0;
    do
    {
        status = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_word);

        if ( rx_bytes >= respLen )
            break;

        usleep(100);

        us_slept += 100 ;

        if ( us_slept > max_us_sleep )
        {
            output("TimeOut #2 \n");
            return FT_OTHER_ERROR;
        }

    } while (rx_bytes < respLen && status == FT_OK);

    status = FT_Read(handle, buffer + AMBE3000_HEADER_LEN , respLen, &rx_bytes);

    if ( status != FT_OK || rx_bytes != respLen )
    {
        output( ANSI_RED "ThumbDV: Process serial. error when reading from the serial port, len = %d, status=%d\n" ANSI_WHITE, rx_bytes, status );
    }

    respLen += AMBE3000_HEADER_LEN;

    BufferDescriptor desc = NULL;
    packet_type = buffer[3];

    //thumbDV_dump("Serial data", buffer, respLen);
    if ( packet_type == AMBE3000_CTRL_PKT_TYPE ) {
        thumbDV_dump( ANSI_YELLOW "Serial data" ANSI_WHITE, buffer, respLen );
    } else if ( packet_type == AMBE3000_CHAN_PKT_TYPE ) {
        desc = hal_BufferRequest( respLen, sizeof( unsigned char ) );
        memcpy( desc->buf_ptr, buffer, respLen );
        //thumbDV_dump(ANSI_BLUE "Coded Packet" ANSI_WHITE, buffer, respLen);
        /* Encoded data */
        _thumbDVEncodedList_LinkTail( desc );
    } else if ( packet_type == AMBE3000_SPEECH_PKT_TYPE ) {
        desc = hal_BufferRequest( respLen, sizeof( unsigned char ) );
        memcpy( desc->buf_ptr, buffer, respLen );
        //thumbDV_dump("SPEECH Packet", buffer, respLen);
        /* Speech data */
        _thumbDVDecodedList_LinkTail( desc );

    } else {
        output( ANSI_RED "Unrecognized packet type 0x%02X ", packet_type );
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
            output( ANSI_YELLOW, "WARNING LENGHT DOESN'T Match %d " ANSI_WHITE, length );
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

    if ( packet_in != NULL && handle != NULL ) {
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
        thumbDV_writeSerial( handle, full_packet, 15 );
        sem_post(&_read_sem);
    }
}

int thumbDV_encode( FT_HANDLE handle, short * speech_in, unsigned char * packet_out, uint8 num_of_samples )
{
    if ( speech_in == NULL || packet_out == NULL || num_of_samples == 0U ) {
        return 0;
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

    if ( handle != NULL )
    {
        thumbDV_writeSerial( handle, packet, length + AMBE3000_HEADER_LEN );
		sem_post(&_read_sem);
    }

    int32 samples_returned = 0;
    BufferDescriptor desc = _thumbDVEncodedList_UnlinkHead();

    if ( desc != NULL ) {
        if ( desc->num_samples > 6U ) {
            uint32 encoded_bytes = desc->sample_size * ( desc->num_samples - 6U );
            if ( encoded_bytes > num_of_samples ) {
                output( ANSI_YELLOW "ThumbDV: clipping encoded AMBE bytes from %u to %u\n" ANSI_WHITE,
                        encoded_bytes, num_of_samples );
                encoded_bytes = num_of_samples;
            }
            if ( encoded_bytes > THUMBDV_DSTAR_AMBE_BYTES ) {
                output( ANSI_YELLOW "ThumbDV: clipping encoded D-Star AMBE bytes from %u to %u\n" ANSI_WHITE,
                        encoded_bytes, THUMBDV_DSTAR_AMBE_BYTES );
                encoded_bytes = THUMBDV_DSTAR_AMBE_BYTES;
            }
            memcpy( packet_out, desc->buf_ptr + 6, encoded_bytes );
            samples_returned = encoded_bytes;
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

static void _connectSerial( FT_HANDLE * ftHandle )
{
    int i = 0 ;

    output("ConnectSerial\n");

    DWORD numDevs = 0;
    FT_DEVICE_LIST_INFO_NODE *devInfo = NULL;
    FT_STATUS status = FT_OK;

    do {

        status = FT_CreateDeviceInfoList(&numDevs);
        if (status != FT_OK)
        {
            output("Unable to create Device Info \n");
        }

        devInfo = (FT_DEVICE_LIST_INFO_NODE *) safe_malloc(sizeof(FT_DEVICE_LIST_INFO_NODE) * numDevs);

        status = FT_GetDeviceInfoList(devInfo, &numDevs);
        if (status != FT_OK)
        {
            output("Unable to fetch Device Info \n");
        }

        for ( i = 0 ; i < numDevs ; i++ )
        {

            *ftHandle = thumbDV_openSerial(devInfo[i]);

            if ( *ftHandle != NULL )
            {
                /* We opened a valid port and detected the ThumbDV */
                break;
            }
        }
        safe_free(devInfo);

        if ( *ftHandle == NULL ) {
            const char * fail_fast = getenv("AETHER_DSTAR_FAIL_FAST");
            if ( fail_fast != NULL && strcmp(fail_fast, "0") != 0 ) {
                output( "Could not open ThumbDV serial device; exiting because fail-fast is enabled.\n" );
                exit(3);
            }
            output( "Could not open serial. Waiting 1 second before trying again.\n" );
            usleep( 1000 * 1000 );
        }
    } while ( *ftHandle == NULL ) ;

    // Reset and Product ID printout are done in thumbDV_openSerial() which calls _check_serial()

    unsigned char disable_parity[6] = {0x61, 0x00, 0x02, 0x00, 0x3F, 0x00};
    thumbDV_writeSerial( *ftHandle, disable_parity, 6 );
    thumbDV_processSerial(*ftHandle);

    unsigned char get_version[5] = {0x61, 0x00, 0x01, 0x00, 0x31};
    unsigned char read_cfg[5] = {0x61, 0x00, 0x01, 0x00, 0x37};
    unsigned char dstar_mode[17] = {0x61, 0x00, 0x0D, 0x00, 0x0A, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};

    thumbDV_writeSerial( *ftHandle, get_version, 5 );
    thumbDV_processSerial(*ftHandle);
    thumbDV_writeSerial( *ftHandle, read_cfg, 5 );
    thumbDV_processSerial(*ftHandle);
    thumbDV_writeSerial( *ftHandle, dstar_mode, 17 );
    thumbDV_processSerial(*ftHandle);

////    /* Init */
    unsigned char pkt_init[6] = { 0x61, 0x00, 0x02, 0x00, 0x0B, 0x07 };
    thumbDV_writeSerial( *ftHandle, pkt_init, 6 );
    thumbDV_processSerial(*ftHandle);

    /* PKT GAIN - set to 0dB */
    unsigned char pkt_gain[7] = { 0x61, 0x00, 0x03, 0x00, 0x4B, 0x00, 0x00 };
    thumbDV_writeSerial( *ftHandle, pkt_gain, 7 );
    thumbDV_processSerial(*ftHandle);

    /* Companding off so it uses 16bit linear */
    unsigned char pkt_compand[6] = { 0x61, 0x00, 0x02, 0x00, 0x32, 0x00 };
    thumbDV_writeSerial( *ftHandle, pkt_compand, 6 );
    thumbDV_processSerial(*ftHandle);

    unsigned char pkt_fmt[7] = {0x61, 0x00, 0x3, 0x00, 0x15, 0x00, 0x00};
    thumbDV_writeSerial( *ftHandle, pkt_fmt, 7 );
    thumbDV_processSerial(*ftHandle);

}

static void * _thumbDV_readThread( void * param )
{
    FT_STATUS status = FT_OK;
    FT_HANDLE handle = *(FT_HANDLE *) param;

    prctl(PR_SET_NAME, "DV-Read");

    while ( !_readThreadAbort )
    {
        sem_wait(&_read_sem);

        if (!allowedToRead)
        {
            break;
        }
        else
        {
            thumbDV_processSerial(handle);
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

    FT_STATUS status = FT_OK;
    FT_HANDLE handle = *(FT_HANDLE *) param;

    while ( !_connectThreadAbort ) {

        //waits 1 second before checking status to prevent CPU hogging
        usleep(1000000);
        ret = FT_GetStatus(handle, &rx_bytes, &tx_bytes, &event_dword);

        if (ret != FT_OK) {

            //clear out read buffer and stop read thread
            sem_post(&_read_sem);
            allowedToRead = FALSE;

            output("Serial is disconnected\n");

             //Set invalid FD in sched_waveform so we don't call write functions
            handle = NULL;
            sched_waveform_setHandle(&handle);
             //This function hangs until a new connection is made
            _connectSerial(&handle);
             //Update the sched_waveform to new valid serial
            sched_waveform_setHandle(&handle);

            //Start read thread again
            allowedToRead = TRUE;
            pthread_create( &_read_thread, NULL, &_thumbDV_readThread, &handle );
        }
    }
}

void thumbDV_init( FT_HANDLE * handle ) {
    pthread_rwlock_init( &_encoded_list_lock, NULL );
    pthread_rwlock_init( &_decoded_list_lock, NULL );

    sem_init(&_read_sem, 0, 0);

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

    _connectSerial(handle);

    pthread_create( &_connect_thread, NULL, &_thumbDV_connectThread, handle );
    pthread_create( &_read_thread, NULL, &_thumbDV_readThread, handle );

    struct sched_param fifo_param;
    fifo_param.sched_priority = 30;
    pthread_setschedparam( _read_thread, SCHED_FIFO, &fifo_param );

}
