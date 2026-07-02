/* *****************************************************************************
 *	discovery_client.c
 *
 *   \brief Discovery Client - Receives and parses discovery packets
 *
 *
 * 	\author Eric Wachsmann, KE5DTO
 * 	\date   2014-09-01
 *
 *******************************************************************************
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
 * ***************************************************************************/

#include <stdio.h>
#include <stdlib.h> // for malloc
#include <string.h> // for memset
#include <sys/socket.h>
#include <netinet/in.h> // for htonl, htons, IPPROTO_UDP
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h> // for errno
#include <unistd.h>

#include <sys/prctl.h>

#include "common.h"
#include "discovery_client.h"
#include "cmd.h" // for tokenize
#include "io_utils.h"


static int _dc_sock;
static pthread_t _dc_thread;
static BOOL _dc_abort = FALSE;
static char * _restrict_ip = NULL;


void printRadio(Radio radio)
{
	output("discovery_protocol_version=%s model=%s serial=%s version=%s nickname=%s callsign=%s ip=%s port=%s status=%s\n",
			radio->discovery_protocol_version,
			radio->model,
			radio->serial,
			radio->version,
			radio->nickname,
			radio->callsign,
			radio->ip,
			radio->port,
			radio->status);
}

	char* protocol_version;
	char* model;
	char* serial;
	char* version;
	char* nickname;
	char* callsign;
	char* ip;
	char* port;
	char* status;

static void _dc_RadioFound(Radio radio)
{

	BOOL radio_found = FALSE;

	if ( _restrict_ip != NULL ) {
		if ( getIP(radio->ip) == getIP(_restrict_ip)) {
			/* We have found the radio that we are restricted to */
			output("We found a radio that maches our _restrict_ip - '%s'\n", _restrict_ip);
			radio_found = TRUE;
		}
	} else if ( FALSE ) { /* Are we running within a radio? */

		if(getIP(radio->ip) == ntohl(net_get_ip())) {
			radio_found = TRUE;
			output("We found a radio that is running on the same box as us - '%s'\n", radio->ip);
		}

	} else { /* Simply connect to first radio we find */
		radio_found = TRUE;
		output("We are attaching to the first radio we see");
	}

	if ( radio_found ) {
		output("Radio found\n");
		// yes -- connect and stop looking for more radios
		// TODO: connect
		    // start a keepalive to keep the channel open and know when it dies
		tc_Init(radio->ip, radio->port);

		usleep(250000);
		hal_Listener_Init();

		dc_Exit();
	}

	// print the content of the object
	//printRadio(radio);

	// because the radio object is malloc'ed, we need to recover the memory
	safe_free(radio);
	radio = 0;
}

static void _dc_ListenerParsePacket(uint8* packet, int32 length, struct sockaddr_in* sender)
{
	//output("_dc_ListenerParsePacket\n");

	// is this packet long enough to inspect for VITA header info?
	if(length < 16)
	{
		// no -- discard the packet
		//output("_dc_ListenerParsePacket: packet too short\n");
		return;
	}

	// cast the incoming packet as a VITA packet
	VitaIFData p = (VitaIFData)packet;

	// does this packet have our OUI?
	if(ntohl(p->class_id_h) != 0x00001C2D)
	{
		// no -- discard this packet
		output("_dc_ListenerParsePacket: wrong OUI (0x%08X)\n", htonl(p->class_id_h));
		return;
	}

	// is this packet an extended data packet?
	if((ntohl(p->header) & VITA_HEADER_PACKET_TYPE_MASK) != VITA_PACKET_TYPE_EXT_DATA_WITH_STREAM_ID)
	{
		// no -- discard this packet
		output("_dc_ListenerParsePacket: wrong packet type (0x%08X)\n", p->header & VITA_HEADER_PACKET_TYPE_MASK);
		return;
	}

	// is this packet marked as a SL_VITA_DISCOVERY_CLASS?
	if((ntohl(p->class_id_l) & VITA_CLASS_ID_PACKET_CLASS_MASK) != 0xFFFF)
	{
		// no -- discard this packet
		output("_dc_ListenerParsePacket: wrong packet class (0x%04X)\n", p->class_id_l & VITA_CLASS_ID_PACKET_CLASS_MASK);
		return;
	}

	// if we made it this far, then we can safely assume this is a
	// discovery packet and we will attempt to split the payload up
	// similar to a command on spaces and parse the data

	int argc, i;
	char *argv[MAX_ARGC + 1];		//Add one extra so we can null terminate the array

	// split the payload string up
	tokenize((char*)p->payload, &argc, argv, MAX_ARGC);

	//output("_dc_ListenerParsePacket: payload: %s\n", p->payload);
	//output("_dc_ListenerParsePacket: tokenize argc=%u\n", argc);

	Radio radio = (Radio)safe_malloc(sizeof(radioType));
	if(!radio)
	{
		output("_dc_ListenerParsePacket: Out of memory!\n");
		return;
	}

	// clear the newly allocated memory
	memset(radio, 0, sizeof(radioType));

	// for each token, process the string
	for(i=0; i<argc; i++)
	{
		if(strncmp(argv[i], "discovery_protocol_version", strlen("discovery_protocol_version")) == 0)
			radio->discovery_protocol_version = argv[i]+strlen("discovery_protocol_version=");
		else if(strncmp(argv[i], "model", strlen("model")) == 0)
			radio->model = argv[i]+strlen("model=");
		else if(strncmp(argv[i], "serial", strlen("serial")) == 0)
			radio->serial = argv[i]+strlen("serial=");
		else if(strncmp(argv[i], "version", strlen("version")) == 0)
			radio->version = argv[i]+strlen("version=");
		else if(strncmp(argv[i], "nickname", strlen("nickname")) == 0)
			radio->nickname = argv[i]+strlen("nickname=");
		else if(strncmp(argv[i], "callsign", strlen("callsign")) == 0)
			radio->callsign = argv[i]+strlen("callsign=");
		else if(strncmp(argv[i], "ip", strlen("ip")) == 0)
			radio->ip = argv[i]+strlen("ip=");
		else if(strncmp(argv[i], "port", strlen("port")) == 0)
			radio->port = argv[i]+strlen("port=");
		else if(strncmp(argv[i], "status", strlen("status")) == 0)
			radio->status = argv[i]+strlen("status=");
	}

	// did we get at least an IP, port, and version?
	if(radio->ip != 0 && radio->port != 0 && radio->version != 0) {
		// yes -- report the radio as found
		_dc_RadioFound(radio);
	} else {
		safe_free(radio);
	}
}

static struct timeval timeout;
//! Allocates a buffer and receives one packet.
//! /param buffer Buffer to be allocated and populated with packet data
//! /returns Number of bytes read if successful, otherwise an error (recvfrom)
static BOOL _dc_ListenerRecv(uint8* buffer, int32* len, struct sockaddr_in* sender_addr)
{
	//output("_dc_ListenerRecv\n");

	uint32 addr_len = sizeof(struct sockaddr_in);

	// we will wait up to 1 second for data in case someone is trying to abort us

	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	fd_set socks;
	FD_ZERO(&socks);
	FD_SET(_dc_sock, &socks);

	// see if there is data in the socket (but timeout if none)
	select(_dc_sock + 1, &socks, NULL, NULL, &timeout);
	if (FD_ISSET(_dc_sock, &socks))
	{
		// yes there is data -- get it
		*len = recvfrom(_dc_sock, buffer, ETH_FRAME_LEN, 0, (struct sockaddr*)sender_addr, &addr_len);
		//precisionTimerLap("HAL Listener Recv");
		if(*len < 0)
			output("_dc_ListenerRecv: recvfrom returned -1  errno=%08X\n", errno);
		//else
			//output("_hal_ListenerRecv: Error len=%d sender=%s:%u\n", len, inet_ntoa(sender_addr.sin_addr), htons(sender_addr.sin_port));

		// TODO: May need to filter here to handle security (packet injection)
		return TRUE;
	}

	*len = 0;
	return FALSE;
}

static void* _dc_ListenerLoop(void* param)
{
	//printf("_dc_ListenerLoop\n");
    prctl(PR_SET_NAME, "DV-ListenerLoop");

	struct sockaddr_in sender;
	uint8 buf[ETH_FRAME_LEN];

	while(!_dc_abort)
	{
		// get some data
		int32 length = 0;
		BOOL success = FALSE;

		while (!success && !_dc_abort)
		{
			memset(&sender,0,sizeof(struct sockaddr_in));
			memset(&buf,0,ETH_FRAME_LEN);
			success = _dc_ListenerRecv(buf, &length, &sender);
		}

		if (!_dc_abort)
		{
			if(length == 0) // socket has been closed
			{
				output("_dc_ListenerLoop error: socket closed\n");
				break;
			}

			if(length < 0)
			{
				output("_dc_ListenerLoop error: loop stopped\n");
				break;
			}

			// length was reasonable -- lets try to parse the packet
			//precisionTimerLap("HAL Listener Parse Packet");
			_dc_ListenerParsePacket(buf, length, &sender);
		}
	}

	if ( _restrict_ip ) {
		safe_free(_restrict_ip);
		_restrict_ip = NULL;
	}

	return NULL;
}

void dc_Init(const char * radio_ip)
{
	output("Discovery Client Init: Opening socket");
	int true = TRUE;
	if((_dc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
	{
		output("...failed! (socket call returned -1)\n");
		return;
	}

	// set up destination address
	struct sockaddr_in addr;

	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY); // this is where we could limit to one IP
	addr.sin_port=htons(DISCOVERY_PORT);

	/* If you're running on the same box as the smartsdr firmware this is necessary so
	 * that both processes can bind to the same VITA port
	 */
	errno = 0;
	setsockopt(_dc_sock, SOL_SOCKET, SO_REUSEADDR, &true, sizeof(true));
	if (errno)
	{
		output("error with reuse option: errno=%d",errno);
	}

	// bind the socket to the port and/or IP
	output("...binding");
	errno = 0;
	if(bind(_dc_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1)
	{
		output("...failed! (bind call returned -1: errno:%d)\n", errno);
		return;
	}
	output("\n");

	if ( _restrict_ip ) {
		safe_free(_restrict_ip );
		_restrict_ip = NULL;
	}

	if ( radio_ip != NULL ) {
		_restrict_ip = safe_malloc(strlen(radio_ip) + 1 );
		strncpy(_restrict_ip, radio_ip, strlen(radio_ip) + 1);
	}

	// start the listener thread
	pthread_create(&_dc_thread, NULL, &_dc_ListenerLoop, NULL);
}

void dc_Exit(void)
{
	_dc_abort = TRUE;
}
