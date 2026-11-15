///*!	\file hal_vita.c
// *	\brief Structure to support VITA-49 packets
// *
// *	\copyright	Copyright 2012-2013 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date 29-MAR-2012
// *	\author Eric & Steve
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
#include <netinet/in.h> // htonl

#include "common.h"
#include "hal_vita.h"

uint32 hal_VitaIFPacketPayloadSize(VitaIFData packet)
{
	uint32 header = htonl(packet->header);
	uint32 bytes = (header & VITA_HEADER_PACKET_SIZE_MASK)*4; // packet size is in 32-bit words

	switch(header & VITA_HEADER_PACKET_TYPE_MASK)
	{
		case VITA_PACKET_TYPE_IF_DATA: // do nothing
			break;
		case VITA_PACKET_TYPE_IF_DATA_WITH_STREAM_ID:
			bytes -= 4; // account for stream ID
			break;
		default: // wrong kind of packet here
			//output("Called with wrong type of packet (%X)\n", header>>28);
			break;
	}

	bytes -= 4; // account for header

	if(header & VITA_HEADER_C_MASK) bytes -= 8; // account for class ID
	if(header & VITA_HEADER_T_MASK) bytes -= 4; // account for trailer

	if((header & VITA_HEADER_TSI_MASK) != VITA_TSI_NONE)
		bytes -= 4;

	if((header & VITA_HEADER_TSF_MASK) != VITA_TSF_NONE)
		bytes -= 8;

	return bytes;
}
