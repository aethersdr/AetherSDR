///*!	\file io_utils.c
// *	\brief Module that contains various IO utilities
// *
// *	\copyright	Copyright 2011-2012 FlexRadio Systems.  All Rights Reserved.
// *				Unauthorized use, duplication or distribution of this software is
// *				strictly prohibited by law.
// *
// *	\date 9-NOV-2011
// *    \author Terry Gerdes, AB5K
// *    \author Stephen Hicks, N5AC
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
#include <sys/types.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "common.h"

uint32 net_get_ip()
{
#ifdef _WIN32
    /* Only used for the inherited banner and an unreachable discovery branch. */
    return 0;
#else
    struct ifaddrs* ifAddrStruct = NULL;
    struct ifaddrs* ifa = NULL;
    uint32 ip = 0;

    if (getifaddrs(&ifAddrStruct) != 0) {
        return 0;
    }

    for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET
            || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        ip = ((struct sockaddr_in*)ifa->ifa_addr)->sin_addr.s_addr;
        break;
    }

    freeifaddrs(ifAddrStruct);

    return ip;
#endif
}
