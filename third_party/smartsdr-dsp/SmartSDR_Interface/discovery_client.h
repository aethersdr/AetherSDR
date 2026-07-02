/* *****************************************************************************
 *	discovery_client.h
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
 * ************************************************************************** */

#ifndef DISCOVERY_CLIENT_H_
#define DISCOVERY_CLIENT_H_

typedef struct _radio
{
	char* discovery_protocol_version;
	char* model;
	char* serial;
	char* version;
	char* nickname;
	char* callsign;
	char* ip;
	char* port;
	char* status;

} radioType, *Radio;

void dc_Init(const char * radio_ip);
void dc_Exit(void);

void printRadio(Radio radio);


#endif // DISCOVERY_CLIENT_H_
