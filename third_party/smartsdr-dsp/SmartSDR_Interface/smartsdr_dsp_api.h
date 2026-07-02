///*    \file smartsdr_dsp_api.c
// *    \brief Main SmartSDR DSP API Entry point
// *
// *    \copyright  Copyright 2011-2013 FlexRadio Systems.  All Rights Reserved.
// *                Unauthorized use, duplication or distribution of this software is
// *                strictly prohibited by law.
// *
// *    \date 31-AUG-2014
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

#ifndef SMARTSDR_DSP_API_H_
#define SMARTSDR_DSP_API_H_

#include "common.h"
#include "traffic_cop.h"

static const int MAX_API_COMMAND_SIZE = 1024;
static const int RECV_BUF_SIZE = 8192;
static const int RECV_BUF_SIZE_TO_GET = 2048;

void api_setVersion(uint32 version);
uint32 api_getVersion(void);
void api_setHandle(uint32 handle);
uint32 api_getHandle(void);
void SmartSDR_API_Shutdown(void);
void SmartSDR_API_Init(BOOL enable_console, const char * radio_ip);
uint32 register_mode(void);


#endif /* SMARTSDR_DSP_API_H_ */
