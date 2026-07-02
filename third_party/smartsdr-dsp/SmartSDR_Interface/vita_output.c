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

#include "vita_output.h"
#include "common.h"
#include "hal_listener.h"

extern int errno;

#define FORMAT_DBFS 0
#define FORMAT_DBM 1

#define VITA_CLASS_ID_1			(uint32)VITA_OUI
#define VITA_CLASS_ID_2			SL_VITA_INFO_CLASS << 16 | SL_VITA_IF_DATA_CLASS

#define MAX_SAMPLES_PER_PACKET	(MAX_IF_DATA_PAYLOAD_SIZE/8)
#define MAX_BINS_PER_PACKET 700

// local variable declarations
static vita_if_data waveform_packet;

static int vita_sock;
static struct sockaddr_in vita_sLocalAddr;
static uint32 _local_ip_addr;
static uint16 _dest_port;

void vita_output_Init(const char * ip )
{
	output("\033[32mInitializing VITA-49 output engine...\n\033[m");

	vita_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (vita_sock < 0)
	{
		output(ANSI_RED " Failed to initialize VITA socket\n");
		return;
	}
	errno = 0;
	bind(vita_sock, (struct sockaddr *)&vita_sLocalAddr, sizeof(vita_sLocalAddr));
	if (errno)
	{
		output(ANSI_RED "error binding socket: errno=%d\n",errno);
	}

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

void UDPSendByIPandPort(void* packet, uint32 num_bytes, uint32 ip_address, uint16 udp_port)
{
	struct sockaddr_in sock;
	sock.sin_family = AF_INET;
	sock.sin_addr.s_addr = htonl(ip_address);
	sock.sin_port = htons(udp_port);
	errno = 0;
	int32 ret_val = sendto(vita_sock, packet, num_bytes, 0, (struct sockaddr*)&sock, sizeof(sock));
	if ( errno || ret_val < 0) {
		output( "Error sending packet: errno%d \n", errno);
	}

}

static void _vita_formatWaveformPacket(Complex* buffer, uint32 samples, uint32 stream_id, uint32 packet_count,
		uint32 class_id_h, uint32 class_id_l, uint32 ip_addr, uint16 port)
{
	waveform_packet.header = htonl(
			VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID |
			VITA_HEADER_CLASS_ID_PRESENT |
			VITA_TSI_OTHER |
			VITA_TSF_SAMPLE_COUNT |
			(packet_count << 16) |
			(7+samples*2));
	waveform_packet.stream_id = htonl(stream_id);
	waveform_packet.class_id_h =  htonl(class_id_h);
	waveform_packet.class_id_l =  htonl(class_id_l);
	waveform_packet.timestamp_int = 0;
	waveform_packet.timestamp_frac_h = 0;
	waveform_packet.timestamp_frac_l = 0;

	memcpy(waveform_packet.payload, buffer, samples * sizeof(Complex));
	//HAL_update_count(stream_id, class_id_h, class_id_l, samples * 8 + 28, HAL_STATUS_OUTPUT_OK, OUTPUT, WFM, ip_addr, port);
}

static uint32 _waveform_packet_count = 0;

void emit_waveform_output(BufferDescriptor buf_desc_out)
{
	int samples_sent, samples_to_send;
	Complex * buf_pointer;

	if (buf_desc_out == NULL)
	{
		output(ANSI_RED "buf_desc_out is NULL\n");
		return;
	}
	if (buf_desc_out->buf_ptr == NULL)
	{
		output(ANSI_RED "buf_desc_out->buf_ptr is NULL\n");
		return;
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
				_waveform_packet_count++ & 0xF,
				(uint32) FLEXRADIO_OUI,
				SL_VITA_SLICE_AUDIO_CLASS,
				_local_ip_addr,
				4991);
		buf_pointer += samples_to_send;
		samples_sent += samples_to_send;
		vita_sLocalAddr.sin_port = htons(VITA_49_SOURCE_PORT);
		UDPSendByIPandPort(&waveform_packet, samples_to_send * 8 + 28, _local_ip_addr, _dest_port);
	}
}
