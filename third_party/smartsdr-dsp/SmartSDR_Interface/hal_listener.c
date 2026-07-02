///*!	\file hal_listener.c
// *	\brief Listener for VITA-49 packets
// *
// *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date 28-MAR-2012
// *	\author Eric & Steve
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
#include <string.h> // for memset
#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h> // for htonl, htons
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h> // for errno
#include <unistd.h>			// for usleep
#include <netdb.h>
#include <sys/prctl.h>

// #define LOG_MODULE LOG_MODULE_HAL_LISTENER

#include "vita.h"
#include "hal_vita.h"
#include "common.h"

#include "stream.h"
#include "vita_output.h"
#include "hal_buffer.h"
#include "sched_waveform.h"

extern int errno;

// static local variables
static int fpga_sock;
static BOOL hal_listen_abort;

static pthread_t _hal_listen_thread;
//static Thread _hal_counter_thread;

// prototypes
static void _hal_ListenerParsePacket(uint8* packet, int32 length, struct sockaddr_in* sender);
static void _hal_ListenerProcessWaveformPacket(VitaIFData p, uint32 stream_id, int32 length, struct sockaddr_in* sender);

#define MAX_COUNTED_STREAMS 100
#define COUNTER_INTERVAL_MS 1000
//static stream_count_type current_counters[MAX_COUNTED_STREAMS];
//static stream_count_type report_counters[MAX_COUNTED_STREAMS];
//static uint32 _end_count, _report_count;

//void HAL_update_count(uint32 stream_id, uint32 class_id_h, uint32 class_id_l, uint32 size, uint32 status, StreamDirection direction, ShortStreamType strm_type, uint32 ip, uint16 port)
//{
//	acquireLocalLock(_counter_lock, LOCAL_LOCK_WRITE);
//	int i;
//	for (i = 0; i < _end_count; i++)
//	{
//		if (stream_id == current_counters[i].stream_id &&
//			class_id_h == current_counters[i].class_id_h &&
//			class_id_l == current_counters[i].class_id_l &&
//			direction == current_counters[i].direction &&
//			strm_type == current_counters[i].stream_type &&
//			ip == current_counters[i].ip &&
//			port == current_counters[i].port)
//		{
//			current_counters[i].count++;
//			current_counters[i].size += size;
//			goto end;
//		}
//	}
//	if (_end_count == MAX_COUNTED_STREAMS) return;
//	current_counters[_end_count].direction = direction;
//	current_counters[_end_count].stream_type = strm_type;
//	current_counters[_end_count].class_id_h = class_id_h;
//	current_counters[_end_count].class_id_l = class_id_l;
//	current_counters[_end_count].count = 1;
//	current_counters[_end_count].size = size;
//	current_counters[_end_count].stream_id = stream_id;
//	current_counters[_end_count].status = status;
//	current_counters[_end_count].ip = ip;
//	current_counters[_end_count].port = port;
//	_end_count++;
//
//end:
//	releaseLocalLock(_counter_lock);
//}

//static void _hal_reset_count(void)
//{
//	int i;
//
////	acquireLocalLock(_print_lock, LOCAL_LOCK_WRITE);
////	acquireLocalLock(_counter_lock, LOCAL_LOCK_WRITE);
//	// make a backup copy of the counters for reporting
//	memcpy(report_counters, current_counters, sizeof(stream_count_type)*_end_count);
//	_report_count = _end_count;
//
//	for (i = 0; i < _report_count; i++)
//	{
//		report_counters[i].speed = (float)report_counters[i].size / COUNTER_INTERVAL_MS * 0.008;
//		report_counters[i].count = (int)(report_counters[i].count * 1000 / COUNTER_INTERVAL_MS);
//		report_counters[i].printed = FALSE;
//	}
//	// clear out the current current_counters
//	memset(current_counters, 0, sizeof(stream_count_type)*_end_count);
//	_end_count = 0;
//	releaseLocalLock(_counter_lock);
//	releaseLocalLock(_print_lock);
//}

//static void* _hal_counter_loop(void* param)
//{
//	// show that we are running
//	thread_setRunning(_hal_counter_thread);
//
//	while (!hal_listen_abort)
//	{
//		// sleep for the designated time
//		usleep (COUNTER_INTERVAL_MS * 1000);
//		_hal_reset_count();
//	}
//
//	destroyLocalLock(&_counter_lock);
//	thread_done(_hal_counter_thread);
//	return NULL;
//}

char* _hal_getStreamType(ShortStreamType strm_type)
{
	switch (strm_type)
	{
		case FFT: return "FFT";
		case MMX: return "MMX";
		case AUD: return "AUD";
		case MET: return "MET";
		case DSC: return "DSC";
		case IQD: return "IQD";
		case TXD: return "TXD";
		case PAN: return "PAN";
		case WFL: return "WFL";
		case WFM: return "WFM";
		case XXX: return "---";
	}
	return "---";
}
//
//void _hal_showStreamLine(uint32 i)
//{
//	char* status;
//	switch(report_counters[i].status)
//	{
//	case HAL_STATUS_PROCESSED:			status = "\033[32mProcessing          \033[m"; break;
//	case HAL_STATUS_INVALID_OUI:		status = "\033[91mInvalid OUI         \033[m"; break;
//	case HAL_STATUS_NO_DESC:			status = "\033[33mNo Descriptor       \033[m"; break;
//	case HAL_STATUS_UNSUPPORTED_SAMP:	status = "\033[31mUnsupported MMX Rate\033[m"; break;
//	case HAL_STATUS_UNSUPPORTED_FFT:	status = "\033[31mUnsupported FFT Rate\033[m"; break;
//	case HAL_STATUS_BAD_TYPE:			status = "\033[33mBad Type            \033[m"; break;
//	case HAL_STATUS_OUTPUT_OK:			status = "\033[32mPacket Sent OK      \033[m"; break;
//	case HAL_STATUS_TX_SKIP:			status = "\033[32mSkipping, in TX     \033[m"; break;
//	case HAL_STATUS_TX_ZERO:			status = "\033[32mSending Zero, in TX \033[m"; break;
//	case HAL_STATUS_WFM_SIZE_WRONG:		status = "\033[91mWFM packet size bad \033[m"; break;
//	case HAL_STATUS_WFM_NO_STREAM:		status = "\033[33mNo WFM stream       \033[m"; break;
//	case HAL_STATUS_UNK_STREAM:			status = "\033[33mUnknown Stream ID   \033[m"; break;
//	default:							status = "\033[35mUnknown             \033[m"; break;
//	}
//
////	if (report_counters[i].direction == INPUT)
////	{
////		output("%s %s 0x%08X   0x%08X %08X   %5u   %s   %6.3f Mbps\n",
////				(report_counters[i].direction == INPUT) ? " IN" : "OUT",
////				_hal_getStreamType(report_counters[i].stream_type),
////				report_counters[i].stream_id,
////				report_counters[i].class_id_h,
////				report_counters[i].class_id_l,
////				report_counters[i].count,
////				status,
////				report_counters[i].speed);
////	}
////	else
//	{
//		uint32 ip = htonl(report_counters[i].ip);
//		uint16 port = htons(report_counters[i].port);
//		if (ip == 0xAC1E0102)
//		{
//		output("%s %s 0x%08X   0x%08X %08X   %5u   %s   %6.3f Mbps   \033[33mFPGA\033[m\n",
//				(report_counters[i].direction == INPUT) ? " IN" : "OUT",
//				_hal_getStreamType(report_counters[i].stream_type),
//				report_counters[i].stream_id,
//				report_counters[i].class_id_h,
//				report_counters[i].class_id_l,
//				report_counters[i].count,
//				status,
//				report_counters[i].speed);
//		}
//		else
//		{
//			output("%s %s 0x%08X   0x%08X %08X   %5u   %s   %6.3f Mbps   %u.%u.%u.%u:%u  ",
//					(report_counters[i].direction == INPUT) ? " IN" : "OUT",
//					_hal_getStreamType(report_counters[i].stream_type),
//					report_counters[i].stream_id,
//					report_counters[i].class_id_h,
//					report_counters[i].class_id_l,
//					report_counters[i].count,
//					status,
//					report_counters[i].speed,
//					ip>>24, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, port);
//
//			//output("(%s)",getHost(ip));
//			//char* program = client_getClientProgram(htonl(report_counters[i].ip), htons(report_counters[i].port));
//			//if (program != NULL) output("  %s",program);
//			output ("\n");
//		}
//	}
//}
//
//void hal_showStreamReport(void)
//{
//	int i;
//	float input_total = 0, output_total = 0;
//	uint32 input_packet_rate = 0, output_packet_rate = 0;
//
////	acquireLocalLock(_print_lock, LOCAL_LOCK_WRITE);
//
//	output("\033[96mDIR TYP Stream ID    Class ID              Count   Status              Rate (includes VITA o/h)\033[m\n");
//
//	// output sorted list of input streams
//	uint32 min_stream_id;
//	do
//	{
//		min_stream_id = 0xFFFFFFFF;
//		for (i = 0; i < _report_count; i++)
//		{
//			if (report_counters[i].direction == INPUT &&
//				!report_counters[i].printed &&
//				report_counters[i].stream_id < min_stream_id)
//			{
//				min_stream_id = report_counters[i].stream_id;
//			}
//		}
//		for (i = 0; i < _report_count; i++)
//		{
//			if ((report_counters[i].stream_id == min_stream_id) && !report_counters[i].printed)
//			{
//				_hal_showStreamLine(i);
//				input_total += report_counters[i].speed;
//				input_packet_rate += report_counters[i].count;
//				report_counters[i].printed = TRUE;
//			}
//		}
//	}
//	while (min_stream_id != 0xFFFFFFFF);
//
//
//	uint32 percent = (uint32)(input_total + 0.5);
//	output("\033[32mAggregate input rate: \033[m%u packets/s, %.3f Mbps (%u%%)\n\n",input_packet_rate,input_total,percent);
//
//
//	do
//	{
//		min_stream_id = 0xFFFFFFFF;
//		for (i = 0; i < _report_count; i++)
//		{
//			if (report_counters[i].direction == OUTPUT &&
//				!report_counters[i].printed &&
//				report_counters[i].stream_id < min_stream_id)
//			{
//				min_stream_id = report_counters[i].stream_id;
//			}
//		}
//		for (i = 0; i < _report_count; i++)
//		{
//			if (report_counters[i].stream_id == min_stream_id && !report_counters[i].printed)
//			{
//				_hal_showStreamLine(i);
//				output_total += report_counters[i].speed;
//				output_packet_rate += report_counters[i].count;
//				report_counters[i].printed = TRUE;
//			}
//		}
//	}
//	while (min_stream_id != 0xFFFFFFFF);
//
//	// clear the print flags
//	for (i = 0; i < _report_count; i++)
//	{
//		report_counters[i].printed = FALSE;
//	}
//
////	releaseLocalLock(_print_lock);
//
//	output("\033[32mAggregate output rate: \033[m%u packets/s, %.3f Mbps\n",output_packet_rate,output_total);
//}
//
//float hal_getStreamRate(uint32 stream_id)
//{
//	int i;
//	for (i = 0; i < _report_count; i++)
//	{
//		if (report_counters[i].stream_id == stream_id)
//		{
//			return report_counters[i].speed;
//		}
//	}
//	return 0.0f;
//
//}


static void _hal_ListenerProcessWaveformPacket(VitaIFData p, uint32 stream_id, int32 length, struct sockaddr_in* sender)
{
	/*TODO: Make the buffer size a define*/

	BufferDescriptor buf_desc;
	buf_desc = hal_BufferRequest(HAL_RX_BUFFER_SIZE, sizeof(Complex));

//	BufferDescriptor buf_desc = s->buf_desc_ptr;
//
//	if(buf_desc == NULL) {
//		output( "buf_desc was null. race?)");
//		return;
//	}
//	/*  set timestamp for start of buffer if necessary */
	if(!buf_desc->timestamp_int)
	{
		buf_desc->timestamp_int = htonl(p->timestamp_int);
		buf_desc->timestamp_frac_h = htonl(p->timestamp_frac_h);
		buf_desc->timestamp_frac_l = htonl(p->timestamp_frac_l);
	}


	// calculate number of samples in the buffer
	uint32 packet_frames = hal_VitaIFPacketPayloadSize(p)/buf_desc->sample_size;

	// verify the frames fit in the actual packet size
	if(packet_frames * 8 > length - 28)
	{
		output( "\033[91mIncoming packet size (%d) smaller than vita header claims\033[m\n", length);
		//HAL_update_count(htonl(p->stream_id), htonl(p->class_id_h), htonl(p->class_id_l),length,HAL_STATUS_WFM_SIZE_WRONG, INPUT, AUD, sender->sin_addr.s_addr, sender->sin_port);
		hal_BufferRelease(&buf_desc);
		return;
	}

	//output("Packet frames = %d\n", packet_frames);

	//HAL_update_count(buf_desc->stream_id, htonl(p->class_id_h), htonl(p->class_id_l), length,HAL_STATUS_PROCESSED, INPUT, AUD, sender->sin_addr.s_addr, sender->sin_port);

	// figure out how many frames it would take to fill the buffer
	//uint32 buf_frames_left = buf_desc->num_samples - s->sample_count;
		// figure out how many frames to copy to the buffer
	uint32 frames_to_copy = packet_frames;

	//output("Buffer mag before memcpy %.6g\n", hal_BufferMag(buf_desc));
	// copy the frames from the packet into the buffer
	memcpy(buf_desc->buf_ptr, // dest
			p->payload, // src
			(size_t)frames_to_copy * buf_desc->sample_size); // number of bytes to copy

	//output("Buffer mag after memcpy %.6g\n", hal_BufferMag(buf_desc));
	/* We now require a full frame to arrive in each packet so we don't handle segmentation of data across packets */
	buf_desc->stream_id = ntohl(p->stream_id);
	// send off the buffer to processor that handles giving it to the correct waveform
	sched_waveform_Schedule(buf_desc);

}


static struct timeval timeout;
//! Allocates a buffer and receives one packet.
//! /param buffer Buffer to be allocated and populated with packet data
//! /returns Number of bytes read if successful, otherwise an error (recvfrom)
static BOOL _hal_ListenerRecv(uint8* buffer, int32* len, struct sockaddr_in* sender_addr)
{
	uint32 addr_len = sizeof(struct sockaddr_in);

	// we will wait up to 1 second for data in case someone is trying to abort us

	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	fd_set socks;
	FD_ZERO(&socks);
	FD_SET(fpga_sock, &socks);

	// see if there is data in the socket (but timeout if none)
	select(fpga_sock + 1, &socks, NULL, NULL, &timeout);
	if (FD_ISSET(fpga_sock, &socks))
	{
		// yes there is data -- get it
		*len = recvfrom(fpga_sock, buffer, ETH_FRAME_LEN, 0, (struct sockaddr*)sender_addr, &addr_len);
		//precisionTimerLap("HAL Listener Recv");
		if(*len < 0)
			output("_hal_ListenerRecv: recvfrom returned -1  errno=%08X\n", errno);
		//else
			//output("_hal_ListenerRecv: Error len=%d sender=%s:%u\n", len, inet_ntoa(sender_addr.sin_addr), htons(sender_addr.sin_port));

		// TODO: May need to filter here to handle security (packet injection)
		return TRUE;
	}

	*len = 0;
	return FALSE;
}

static void* _hal_ListenerLoop(void* param)
{
	// show that we are running
//	thread_setRunning(_hal_listen_thread);

	struct sockaddr_in sender;
	uint8 buf[ETH_FRAME_LEN];
	prctl(PR_SET_NAME, "DV-halListener");

	while(!hal_listen_abort)
	{
		// get some data
		int32 length = 0;
		BOOL success = FALSE;

		while (!success && !hal_listen_abort)
		{
			memset(&sender,0,sizeof(struct sockaddr_in));
			memset(&buf,0,ETH_FRAME_LEN);
			success = _hal_ListenerRecv(buf, &length, &sender);
		}

		if (!hal_listen_abort)
		{
			if(length == 0) // socket has been closed
			{
				output("_hal_ListenerLoop error: socket closed\n");
				break;
			}

			if(length < 0)
			{
				output("_hal_ListenerLoop error: loop stopped\n");
				break;
			}

			// length was reasonable -- lets try to parse the packet
			//precisionTimerLap("HAL Listener Parse Packet");
			_hal_ListenerParsePacket(buf, length, &sender);
		}
	}

	//thread_done(_hal_listen_thread);
	return NULL;
}

static void _hal_ListenerParsePacket(uint8* packet, int32 length, struct sockaddr_in* sender)
{
	//Stream s;

	// make sure packet is long enough to inspect for VITA header info
	if(length < 28)
		return;

	VitaIFData p = (VitaIFData) packet;

	// does this packet have our OUI?
	if(htonl(p->class_id_h) != 0x00001C2D)
	{
		//HAL_update_count(htonl(p->stream_id), htonl(p->class_id_h), htonl(p->class_id_l),length,HAL_STATUS_INVALID_OUI, INPUT, XXX, sender->sin_addr.s_addr, sender->sin_port);
		return;
	}

	// verify the length of the packet matches the VITA header
	//uint32 vita_length = (htonl(p->header) & VITA_HEADER_PACKET_SIZE_MASK) * 4;
	/*
	if(vita_length != length - vitaPacketOffset)
	{
		output("_hal_ListenerParsePacket: Vita packet size (%u*4=%u) and packet length (%u) do not match\n",
				vita_length/4, vita_length, length - vitaPacketOffset);
		return;
	}
	*/

	// what kind of Vita Packet is this?

	//if(!(htonl(p->header) & VITA_HEADER_PACKET_TYPE_MASK) == VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID)
	//		output("_hal_ListenerParsePacket: received ext data packet from unexpected packet tpe (0x%08X)\n", htonl(p->header) & VITA_HEADER_PACKET_TYPE_MASK);

//	lockBits locks;

	switch(htonl(p->stream_id) & STREAM_BITS_MASK)
	{
		case STREAM_BITS_WAVEFORM | STREAM_BITS_IN:

			//output("Received a STREAM_BITS_WAVEFORM | STREAM_BITS_IN buffer!!! wooo!!!!");
//			if ( s->buf_desc_ptr == NULL)
//			{
//				HAL_update_count(htonl(p->stream_id), htonl(p->class_id_h), htonl(p->class_id_l),length,HAL_STATUS_NO_DESC, INPUT, WFM, sender->sin_addr.s_addr, sender->sin_port);
//			}
//			else
//			{
				_hal_ListenerProcessWaveformPacket(p, p->stream_id, length, sender);
//			}

		break;

		default:
		//	HAL_update_count(htonl(p->stream_id), htonl(p->class_id_h), htonl(p->class_id_l),length,HAL_STATUS_UNK_STREAM, INPUT, XXX, sender->sin_addr.s_addr, sender->sin_port);
			output("Undefined stream in %08X", p->stream_id);
			break;
	}
}

void hal_Listener_Init(void)
{
	output("Vita Listener Init: Opening socket");

	errno = 0;
	if((fpga_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	{
		output("...failed! (socket call returned -1) - errno = %d\n", errno);
		return;
	}

	// set up destination address
	struct sockaddr_in addr;

	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // this is where we could limit to one IP
	addr.sin_port=htons(VITA_49_PORT);

	// bind the socket to the port and/or IP
	output("...binding");
	if(bind(fpga_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
		output("...failed! (bind call returned -1)\n");
		return;
	}
	output("\n");

	_hal_listen_thread = (pthread_t) NULL;
	pthread_create(&_hal_listen_thread, NULL, &_hal_ListenerLoop, NULL);

//	_hal_counter_thread = NULL;
//	_hal_counter_thread = thread_add(locks, "HALSTATS", "Monitors System I/O", _hal_counter_loop, SCHED_OTHER, 0);
//	if (action == THREAD_START)
//	{
//		thread_start(locks, _hal_counter_thread, NULL);
//	}

}
