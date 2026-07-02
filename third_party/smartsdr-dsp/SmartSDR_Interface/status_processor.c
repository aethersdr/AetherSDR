///*    \file status_processor.c
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "traffic_cop.h"
#include "sched_waveform.h"

static char* _status_value( char * token, const char * key )
{
    size_t key_len = strlen( key );
    if ( token == NULL || strncmp( token, key, key_len ) != 0 || token[key_len] != '=' ) {
        return NULL;
    }

    return token + key_len + 1;
}

static void _handle_status(char* string)
{
    int argc;
    uint32 slc; // slice number
    char *argv[MAX_ARGC_STATUS + 1];       //Add one extra so we can null terminate the array

    // get the actual status message -- we don't care about the handle
    char* save = 0;
    char* start = strtok_r(string,"|",&save);
    start = strtok_r(NULL,"|",&save);
    if ( start == NULL ) {
        return;
    }

    // first let's look for a slice status -- these are most important
    if (strncmp(start, "slice", strlen("slice")) == 0)
    {
        tokenize(start, &argc, argv, MAX_ARGC);

        if (argc < 3)
        {
            // bad slice status ... ignoring it
            return;
        }

        errno = 0;
        slc = strtoul(argv[1], NULL, 0);
        if(errno)
        {
            output(ANSI_RED "Unable to parse slice number (%s)\n", argv[1]);
            return;
        }
        int i;
        for (i = 2; i < argc; i++)
        {
            char* smode = _status_value( argv[i], "mode" );
            if(smode != NULL)
            {
                errno = 0;
                if (strncmp(smode,"DSTR", strlen("DSTR")) == 0)
                {
                    // we are now in DSTR mode
                    output(ANSI_MAGENTA "slice %d is now in DSTR mode\n",slc);
                    char cmd[512] = {0};
                    snprintf(cmd, sizeof(cmd), "slice s %d fm_deviation=1200 post_demod_low=0 post_demod_high=6000 dfm_pre_de_emphasis=0 post_demod_bypass=1 squelch=0", slc);
                    tc_sendSmartSDRcommand(cmd,FALSE, NULL);

                    sched_waveform_setDSTARSlice(slc);

                }
                else
                {
                    // we have left DSTR mode
                    output(ANSI_MAGENTA "slice %d is in %s mode\n",slc,smode);
                }
            }
            char* in_use_value = _status_value( argv[i], "in_use" );
            if(in_use_value != NULL)
            {
                errno = 0;
                int in_use = strtoul(in_use_value, NULL, 0);
                if (!in_use)
                {
                    output(ANSI_MAGENTA "slice %d has been removed\n",slc);

                }
            }
            char* tx_value = _status_value( argv[i], "tx" );
            if(tx_value != NULL)
            {
                errno = 0;
                int tx = strtoul(tx_value, NULL, 0);
                if (tx)
                {
                    output(ANSI_MAGENTA "slice %d is the transmit slice\n",slc);
                }
                else
                {
                    output(ANSI_MAGENTA "slice %d is NOT transmit slice\n",slc);
                }
            }
        }

    }
    else if (strncmp(start, "interlock", strlen("interlock")) == 0)
    {
        tokenize(start, &argc, argv, MAX_ARGC);

        if (argc < 2)
        {
            // bad interlock status ... ignoring it
            return;
        }

        int i;
        for (i = 1; i < argc; i++)
        {
            char* state = _status_value( argv[i], "state" );
            if(state != NULL)
            {
                errno = 0;
                if (strncmp(state,"PTT_REQUESTED",strlen("PTT_REQUESTED")) == 0)
                {
                    output(ANSI_MAGENTA "we are transmitting\n");
                }
                else if (strncmp(state,"READY",strlen("READY")) == 0 ||
                    strncmp(state,"NOT_READY",strlen("NOT_READY")) == 0)
                {
                    output(ANSI_MAGENTA "we are receiving\n");
                }
                else if ( strncmp(state, "UNKEY_REQUESTED", strlen("UNKEY_REQUESTED")) == 0 )
                {
                    output(ANSI_MAGENTA "unkey requested - sending end bits\n");
                    sched_waveform_setEndOfTX(TRUE);
                }

            }
        }

    }

    // now we could check for other statuses that were interesting


}


void status_processor(char* string)
{
    switch (*string)
    {
        case 'V': // version
        {
            string++;
            uint32 version = getIP(string);
            api_setVersion(version);
            break;
        }
        case 'H': // handle
        {
            string++;
            uint32 val;
            sscanf(string, "%08X", &val);
            api_setHandle(val);
            break;
        }
        case 'R': // response
        {
            char* save = NULL;
            string++;
            uint32 val;
            sscanf(string, "%i", &val);
            char* response = strtok_r(string, "|", &save);
            response = strtok_r(NULL, "", &save);
            tc_commandList_respond(val, response);
            break;
        }
        case 'C': // command
        {
            char* save = NULL;
            string++;
            uint32 val;
            sscanf(string, "%i", &val);
            char* cmd = strtok_r(string, "|", &save);
            cmd = strtok_r(NULL, "", &save);
#ifdef DEBUG
            output("\033[32mExecuting command from SmartSDR: \033[m%s\n",cmd);
#endif
            uint32 ret = process_command(cmd);
            char response[1024];
            sprintf(response, "waveform response %d|%d", val, ret);
            tc_sendSmartSDRcommand(response, FALSE, NULL );
            break;
        }
        case 'S': // status
        {
            // here we translate from SmartSDR status message we are interested in
            // and the corresponding commands we want to execute
            _handle_status(string);
            break;
        }
        case 'M': // message

            break;
        default:
            output(ANSI_YELLOW "Status Processor: unknown status \033[m%s\n",string);
            break;
    }
}
