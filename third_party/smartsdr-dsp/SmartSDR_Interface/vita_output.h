///*!	\file vita_output.h
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
#ifndef VITA_OUTPUT_H_
#define VITA_OUTPUT_H_

#include <sys/socket.h>
#include <netinet/in.h>

#include "common.h"
#include "complex.h"
#include "hal_buffer.h"

void vita_output_Init(const char * ip );

void UDPSendByIPandPort(void* packet, uint32 num_bytes, uint32 ip_address, uint16 udp_port);

void emit_waveform_output(BufferDescriptor buf_desc_out);


#endif /* VITA_OUTPUT_H_ */
