///*!	\file vita_output.c
// *	\brief transmit vita packets to the Ethernet
// *
// *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date 2-APR-2012
// *	\author Stephen Hicks, N5AC
// *
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

#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h> // for write, usleep
#include <errno.h>
#include <sys/time.h>

#include "vita_output.h"
#include "common.h"
#include "hal_listener.h"

#define FORMAT_DBFS 0
#define FORMAT_DBM 1

#define VITA_CLASS_ID_1			(uint32)VITA_OUI
#define VITA_CLASS_ID_2			SL_VITA_INFO_CLASS << 16 | SL_VITA_IF_DATA_CLASS

#define MAX_SAMPLES_PER_PACKET	(MAX_IF_DATA_PAYLOAD_SIZE/8)
#define MAX_BINS_PER_PACKET 700
#define PACKET_COUNT_STREAM_SLOTS 64U

// local variable declarations
static vita_if_data waveform_packet;

static uint32 _local_ip_addr;
static uint16 _dest_port;

typedef struct _packet_count_entry {
	uint32 stream_id;
	uint32 next_count;
	BOOL in_use;
} packet_count_entry;

static packet_count_entry _packet_counts[PACKET_COUNT_STREAM_SLOTS];

void vita_output_resetPacketCounts(void)
{
	memset(_packet_counts, 0, sizeof(_packet_counts));
}

uint32 vita_output_nextPacketCount(uint32 stream_id)
{
	uint32 free_slot = PACKET_COUNT_STREAM_SLOTS;
	uint32 i;

	for (i = 0; i < PACKET_COUNT_STREAM_SLOTS; i++)
	{
		if (_packet_counts[i].in_use && _packet_counts[i].stream_id == stream_id)
		{
			const uint32 count = _packet_counts[i].next_count;
			_packet_counts[i].next_count = (count + 1U) & 0xFU;
			return count;
		}
		if (!_packet_counts[i].in_use && free_slot == PACKET_COUNT_STREAM_SLOTS)
			free_slot = i;
	}

	if (free_slot == PACKET_COUNT_STREAM_SLOTS)
		free_slot = stream_id % PACKET_COUNT_STREAM_SLOTS;

	_packet_counts[free_slot].in_use = TRUE;
	_packet_counts[free_slot].stream_id = stream_id;
	_packet_counts[free_slot].next_count = 1U;
	return 0U;
}

void vita_output_Init(const char * ip )
{
	output("\033[32mInitializing VITA-49 output engine on listener socket...\n\033[m");
	vita_output_resetPacketCounts();

	struct in_addr addr;

	if ( ip == NULL ) {
		output(ANSI_RED "NULL IP Supplied!!\n");
		tc_abort();
	}
	if ( inet_aton(ip, &addr) == 0) {
		output(ANSI_RED "Could not convert local addr to binary\n");
	} else {
		_local_ip_addr = ntohl(addr.s_addr);
	}
	_dest_port = 4991;
	// output("host = %d.%d.%d.%d : %d", ip>>24, (ip>>16)&0xFF, (ip>>8)&0xFF, ip&0xFF, _dest_port);

	output("Vita Output Init - ip = '%s' port = %d\n", ip,_dest_port);
}

BOOL UDPSendByIPandPort(void* packet, uint32 num_bytes, uint32 ip_address, uint16 udp_port)
{
	return hal_ListenerSend(packet, num_bytes, ip_address, udp_port);
}

static void _vita_formatWaveformPacket(Complex* buffer, uint32 samples, uint32 stream_id, uint32 packet_count,
		uint32 class_id_h, uint32 class_id_l, uint32 ip_addr, uint16 port)
{
	struct timeval now;
	memset(&now, 0, sizeof(now));
	gettimeofday(&now, NULL);
	const uint64 fractional_picoseconds = (uint64)now.tv_usec * 1000000ULL;

	waveform_packet.header = htonl(
			VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID |
			VITA_HEADER_CLASS_ID_PRESENT |
			VITA_TSI_UTC |
			VITA_TSF_REAL_TIME |
			(packet_count << 16) |
			(7+samples*2));
	waveform_packet.stream_id = htonl(stream_id);
	waveform_packet.class_id_h =  htonl(class_id_h);
	waveform_packet.class_id_l =  htonl(class_id_l);
	waveform_packet.timestamp_int = htonl((uint32)now.tv_sec);
	waveform_packet.timestamp_frac_h = htonl((uint32)(fractional_picoseconds >> 32U));
	waveform_packet.timestamp_frac_l = htonl((uint32)fractional_picoseconds);

	memcpy(waveform_packet.payload, buffer, samples * sizeof(Complex));
	//HAL_update_count(stream_id, class_id_h, class_id_l, samples * 8 + 28, HAL_STATUS_OUTPUT_OK, OUTPUT, WFM, ip_addr, port);
}

BOOL emit_waveform_output(BufferDescriptor buf_desc_out)
{
	int samples_sent, samples_to_send;
	Complex * buf_pointer;
	BOOL success = TRUE;

	if (buf_desc_out == NULL)
	{
		output(ANSI_RED "buf_desc_out is NULL\n");
		return FALSE;
	}
	if (buf_desc_out->buf_ptr == NULL)
	{
		output(ANSI_RED "buf_desc_out->buf_ptr is NULL\n");
		return FALSE;
	}

	Complex* out_buffer = (Complex*)buf_desc_out->buf_ptr;
	uint32 buf_size = buf_desc_out->num_samples;


	// convert to big endian for network
	int i;
	for(i=0; i<buf_size; i++)
	{
		*(uint32*)&out_buffer[i].real = htonl(*(uint32*)&out_buffer[i].real);
		*(uint32*)&out_buffer[i].imag = htonl(*(uint32*)&out_buffer[i].imag);
	}

	samples_sent = 0;
	buf_pointer = out_buffer;
	uint32 preferred_samples_per_packet = buf_size;
	//output("samples_to_send: %d\n", preferred_samples_per_packet);

	while (samples_sent < buf_size)
	{
		if ((buf_size - samples_sent) > preferred_samples_per_packet)
		{
			samples_to_send = preferred_samples_per_packet;
		}
		else
		{
			samples_to_send =  buf_size - samples_sent;
		}
		//output("samples_to_send: %d\n", samples_to_send);
		_vita_formatWaveformPacket(
				buf_pointer,
				samples_to_send,
				buf_desc_out->stream_id,
					vita_output_nextPacketCount(buf_desc_out->stream_id),
				(uint32) FLEXRADIO_OUI,
				SL_VITA_SLICE_AUDIO_CLASS,
				_local_ip_addr,
				4991);
		buf_pointer += samples_to_send;
		samples_sent += samples_to_send;
		if (!UDPSendByIPandPort(&waveform_packet,
		                         samples_to_send * 8 + 28,
		                         _local_ip_addr,
		                         _dest_port))
		{
			success = FALSE;
		}
	}
	return success;
}
