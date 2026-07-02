///*    \file traffic_cop.h
// *    \brief TCP Communications Server
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

#ifndef TRAFFIC_COP_H_
#define TRAFFIC_COP_H_

#include <semaphore.h>
#include "datatypes.h"
#include <time.h>

typedef struct _recieve
{
    char* recv_buf;
    char* read_ptr;
    uint32 buf_len;
    char* command;
    BOOL keepalive_enabled;
    struct timespec last_ping;
} receive_data;

typedef struct _cmd
{
    uint32 sequence;
    sem_t semaphore;
    char* response;
    struct _cmd *next;
    struct _cmd *prev;
} command_type, *Command;

//! ask the TCP/IP command client to abort
void tc_startKeepalive(void);
void tc_abort(void);
void tc_Init(const char * hostname, const char * api_port);

uint32 tc_sendSmartSDRcommand(char* command, BOOL block, char** response);
Command tc_commandList_respond(uint32 sequence, char* response);

#endif /* TRAFFIC_COP_H_ */
