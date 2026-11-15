///*!   \file thumbdv.h
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


#ifndef THUMBDV_THUMBDV_H_
#define THUMBDV_THUMBDV_H_

#include "datatypes.h"
#include "ftd2xx.h"

void thumbDV_init( FT_HANDLE * serial_fd );
FT_HANDLE thumbDV_openSerial( FT_DEVICE_LIST_INFO_NODE device );
int thumbDV_probeConfiguredSerial( void );
const char * thumbDV_lastError( void );
int thumbDV_processSerial( FT_HANDLE handle );

int thumbDV_encode( FT_HANDLE handle, short * speech_in, unsigned char * packet_out, uint8 num_of_samples );
BOOL thumbDV_submitEncode( FT_HANDLE handle,
                           const short * speech_in,
                           uint8 num_of_samples,
                           uint64 correlation_id );
int thumbDV_takeEncoded( unsigned char * packet_out,
                         uint8 packet_capacity,
                         uint64 * correlation_id );
uint32 thumbDV_pendingEncodeRequests( void );
uint32 thumbDV_availableEncodedResponses( void );
uint32 thumbDV_encodeOutstanding( void );
void thumbDV_decode( FT_HANDLE handle, unsigned char * packet_in, uint8 bytes_in_packet );

void thumbDV_dump( char * text, unsigned char * data, unsigned int length );
void thumbDV_flushLists(void);

BOOL thumbDV_getDecodeListBuffering(void);
int thumbDV_unlinkAudio(short * speech_out);

#ifdef AETHER_DSTAR_TESTING
void thumbDV_testReportSerialFault(const char * detail);
BOOL thumbDV_testTakeSerialFault(char * detail, uint32 detail_size);
void thumbDV_testInitializeQueues(void);
void thumbDV_testQueueEncodedFrames(uint32 count);
void thumbDV_testQueueDecodedFrames(uint32 count);
void thumbDV_testEnqueueEncodedRequest(void);
void thumbDV_testEnqueueEncodedRequestWithCorrelation(uint64 correlation_id);
void thumbDV_testEnqueueDecodedRequest(void);
BOOL thumbDV_testConsumeEncodedResponse(void);
BOOL thumbDV_testConsumeDecodedResponse(void);
BOOL thumbDV_testQueueEncodedResponse(void);
BOOL thumbDV_testQueueDecodedResponse(void);
uint64 thumbDV_testEncodedHeadCorrelation(void);
uint32 thumbDV_testPendingEncodedRequests(void);
uint32 thumbDV_testEncodeOutstanding(void);
uint32 thumbDV_testEncodedFrameCount(void);
uint32 thumbDV_testDecodedFrameCount(void);
BOOL thumbDV_testEncodedBuffering(void);
BOOL thumbDV_testDecodedBuffering(void);
BOOL thumbDV_testFailFastPolicy(BOOL initial_connection, const char * value);
BOOL thumbDV_testReadSemaphoreReset(void);
void thumbDV_testDestroyQueues(void);
#endif
#endif /* THUMBDV_THUMBDV_ */
