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

#include "ftd2xx.h"

void thumbDV_init( FT_HANDLE * serial_fd );
FT_HANDLE thumbDV_openSerial( FT_DEVICE_LIST_INFO_NODE device );
int thumbDV_processSerial( FT_HANDLE handle );

int thumbDV_encode( FT_HANDLE handle, short * speech_in, unsigned char * packet_out, uint8 num_of_samples );
void thumbDV_decode( FT_HANDLE handle, unsigned char * packet_in, uint8 bytes_in_packet );

void thumbDV_dump( char * text, unsigned char * data, unsigned int length );
void thumbDV_flushLists(void);

BOOL thumbDV_getDecodeListBuffering(void);
int thumbDV_unlinkAudio(short * speech_out);
#endif /* THUMBDV_THUMBDV_ */
