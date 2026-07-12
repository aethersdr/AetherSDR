/* *****************************************************************************
 *	cmd_basics.c													2014 AUG 30
 *
 *		Uses header file cmd.h
 *
 *   	Basic commands for the command_engine
 *			Display the sign-on banner
 *			Clear Screen
 *			Process Exit
 *			Display time
 *			Display date
 *			Display help
 *			Display "undefined"
 *
 *
 *		\author Terry Gerdes, AB5K
 *		\author Stephen Hicks, N5AC
 *		\author Graham / KE9H
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



#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

#include "common.h"
#include "aether_smartsdr_command.h"
#include "main.h"
#include "cmd.h"

#include "main.h"
#include "sched_waveform.h"
#include "status_processor.h"


/* *****************************************************************************
 *	uint32 cmd_banner(void)
 *
 *		Print a banner
 *
 */

uint32 cmd_banner()
{
	char *build_date = __DATE__;
	char *build_time = __TIME__;
	uint32 ip = net_get_ip();

	output(ANSI_GREEN "*\n");
    output("*  This program is free software: you can redistribute it and/or modify\n");
    output("*  it under the terms of the GNU General Public License as published by\n");
    output("*  the Free Software Foundation, either version 3 of the License, or\n");
    output("*  (at your option) any later version.\n");
    output("*  This program is distributed in the hope that it will be useful,\n");
    output("*  but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
    output("*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the\n");
    output("*  GNU General Public License for more details.\n");
    output("*  You should have received a copy of the GNU General Public License\n");
    output("*  along with this program. If not, see <http://www.gnu.org/licenses/>.\n*\n");
    output("*  Contact Information:\n");
    output("*  email: gpl<at>flexradiosystems.com\n");
    output("*  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728\n*\n");

    output("\033[92m");
	output("**************************************************************************\r\n");
	output("*                                                                          \r\n");
	output("*   *  *  *    *   *       *  ******   ******   ****   *****    **   **       \r\n");
	output("*   *  *  *   * *   *     *   *        *       *    *  *    *   * * * *       \r\n");
	output("*   *  *  *  *****   *   *    *****    ****    *    *  ****     *  *  *       \r\n");
	output("*    ** **  *     *   * *     *        *       *    *  *   *    *     *       \r\n");
	output("*     * *  *       *   *      ******   *        ****   *    *   *     *       \r\n");
	output("*\r\n");
	output("*  FlexRadio Systems\r\n");
	output("*  Copyright (C) 2014 FlexRadio Systems.  All Rights Reserved.\r\n");
	output("*  www.flexradio.com\r\n");
	output("**************************************************************************\r\n");

	//output("\033[32mSoftware version : \033[m%s\r\n", software_version);
	output("\033[32mBuild Date & Time: \033[m%s %s\r\n",build_date, build_time);
	output("\033[32mIP Address       : \033[m%d.%d.%d.%d\r\n", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
	output("\033[32mType <help> for options\r\n\n\033[m");

	return SUCCESS;
}


/* *****************************************************************************
 *	uint32 cmd_cls(int requester_fd, int argc,char **argv)
 *
 *		HANDLE:  command_cls
 *		ANSI escape sequence to go to home position in screen
 *			and clear remainder of screen
 */

uint32 cmd_cls(int requester_fd, int argc,char **argv)
{
    static char *CLS = "\033[H\033[2J";
	write(requester_fd, CLS, strlen(CLS));
	return SUCCESS;
}


/* *****************************************************************************
 *	uint32 cmd_exit(int requester_fd, int argc,char **argv)
 *
 *		Exit the application
 */

uint32 cmd_exit(int requester_fd, int argc,char **argv)
{
	const char string1[] = "\n\033[92m73 de WaveForm !!!\033[m\n";

	write(requester_fd, string1, strlen(string1));

	_exit(0);
	return SUCCESS;
}


/* *****************************************************************************
 *	uint32 cmd_time(int requester_fd, int argc,char **argv)
 *
 *		Display the time
 */

uint32 cmd_time(int requester_fd, int argc,char **argv)
{
    time_t t = time(NULL);
    struct tm time = *localtime(&t);

//   client_response(SUCCESS,"%02d:%02d:%02dZ",time.tm_hour,time.tm_min,time.tm_sec);
    output("%02d:%02d:%02dZ \n",time.tm_hour,time.tm_min,time.tm_sec);
//	char *time_string = 0;
//
//	time_string = drv_Bq32000AsciiTime();
//	strcat(time_string,"\n");
//
//	write(requester_fd, time_string, strlen(time_string));
	return SUCCESS;
}


/* *****************************************************************************
 *	uint32 cmd_date(int requester_fd, int argc,char **argv)
 *
 *		Display the date
 */

uint32 cmd_date(int requester_fd, int argc,char **argv)
{
    time_t t = time(NULL);
    struct tm time = *localtime(&t);

//   client_response(SUCCESS,"%d-%d-%d",time.tm_year+1900,time.tm_mon+1,time.tm_mday);
    output("%d-%d-%d \n",time.tm_year+1900,time.tm_mon+1,time.tm_mday);
//	char *time_string = 0;
//
//	time_string = drv_Bq32000AsciiDatetime();
//	strcat(time_string,"\n");
//
//	write(requester_fd, time_string, strlen(time_string));
	return SUCCESS;
}

//
// Command Description displayed from HELP menu.
//

const char* commandDescriptionBasic[]  =
{
	0,
	"b             Display banner",
	"banner        Display the WaveForm banner",
	"cls           Clear screen",
	"date          Display the Date",
	"exit          Exit the process",
	"quit          Exit the process",
	"time          Display the Time",
	"help|?        View this menu",
  0
};


/* *****************************************************************************
 *	uint32 cmd_help(int requester_fd, int argc, char **argv)
 *
 *		HANDLE:  help
 */

uint32 cmd_help(int requester_fd, int argc, char **argv)
{
	int i;

    i=1;

    output("==========================================================\n\r");
    while(commandDescriptionBasic[i] != 0)
    {
        write(requester_fd, commandDescriptionBasic[i], strlen(
                commandDescriptionBasic[i]));
        output("  %s\n\r", commandDescriptionBasic[i++]);
    }

    return SUCCESS;
}


/* *****************************************************************************
 *	uint32 cmd_undefined(int requester_fd, int argc, char **argv)
 *
 *		Undefined
 */

uint32 cmd_undefined(int requester_fd, int argc, char **argv)
{
    //debug(LOG_CERROR, TRUE, SL_R_UNKNOWN_COMMAND);
	//client_response(SL_UNKNOWN_COMMAND, NULL);
	output("I have no idea what you are talking about !!!\n");
	return SL_UNKNOWN_COMMAND;
}

static const char* _cmd_value(const char* token, const char* key)
{
    const size_t key_length = strlen(key);
    return token != NULL
            && strncmp(token, key, key_length) == 0
            && token[key_length] == '='
        ? token + key_length + 1U
        : NULL;
}

static BOOL _cmd_uint32(const char* value, uint32* parsed)
{
    const char* end = NULL;
    if (aether_smartsdr_parse_uint32(value, 0, parsed, &end) != 0
        || *end != '\0') {
        return FALSE;
    }
    return TRUE;
}

static BOOL _cmd_owner(int argc,
                       char** argv,
                       int first,
                       BOOL* owner_valid,
                       uint32* owner)
{
    *owner_valid = FALSE;
    *owner = 0U;
    for (int i = first; i < argc; i++) {
        const char* value = _cmd_value(argv[i], "owner");
        if (value == NULL) {
            continue;
        }
        if (*owner_valid || !_cmd_uint32(value, owner) || *owner == 0U) {
            return FALSE;
        }
        *owner_valid = TRUE;
    }
    return TRUE;
}

uint32 cmd_slice(int requester_fd, int argc, char **argv)
{
    uint32 slc = INVALID_SLICE_RX;

    if (strcmp(argv[0], "slice") == 0)
    {
        if(argc < 3)
        {
            return SL_BAD_COMMAND;
        }

        // get the slice number
        if(!_cmd_uint32(argv[1], &slc))
        {
            output(ANSI_RED "Unable to parse slice number (%s)\n", argv[1]);
            return SL_BAD_COMMAND;
        }

        if(strcmp(argv[2], "set") == 0)
        {
            BOOL owner_valid = FALSE;
            uint32 owner = 0U;
            if (!_cmd_owner(argc, argv, 3, &owner_valid, &owner)) {
                return SL_BAD_COMMAND;
            }
            enum {
                DESTINATION_RPTR_SEEN = 1U << 0,
                DEPARTURE_RPTR_SEEN = 1U << 1,
                COMPANION_CALL_SEEN = 1U << 2,
                OWN_CALL1_SEEN = 1U << 3,
                OWN_CALL2_SEEN = 1U << 4,
                MESSAGE_SEEN = 1U << 5,
                ALL_DSTAR_FIELDS_SEEN = (1U << 6) - 1U
            };
            uint32 seen = 0U;
            const char* destination_rptr = NULL;
            const char* departure_rptr = NULL;
            const char* companion_call = NULL;
            const char* own_call1 = NULL;
            const char* own_call2 = NULL;
            const char* message = NULL;
            for (int i = 3; i < argc; i++) {
                const char* value = NULL;
                if ( ( value = _cmd_value(argv[i], "destination_rptr") ) != NULL ) {
                    if ((seen & DESTINATION_RPTR_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= DESTINATION_RPTR_SEEN;
                    destination_rptr = value;
                } else if ( ( value = _cmd_value(argv[i], "departure_rptr") ) != NULL ) {
                    if ((seen & DEPARTURE_RPTR_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= DEPARTURE_RPTR_SEEN;
                    departure_rptr = value;
                } else if ( ( value = _cmd_value(argv[i], "companion_call") ) != NULL ) {
                    if ((seen & COMPANION_CALL_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= COMPANION_CALL_SEEN;
                    companion_call = value;
                } else if ( ( value = _cmd_value(argv[i], "own_call1") ) != NULL ) {
                    if ((seen & OWN_CALL1_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= OWN_CALL1_SEEN;
                    own_call1 = value;
                } else if ( ( value = _cmd_value(argv[i], "own_call2") ) != NULL ) {
                    if ((seen & OWN_CALL2_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= OWN_CALL2_SEEN;
                    own_call2 = value;
                } else if ( ( value = _cmd_value(argv[i], "message") ) != NULL ) {
                    if ((seen & MESSAGE_SEEN) != 0U) {
                        return SL_BAD_COMMAND;
                    }
                    seen |= MESSAGE_SEEN;
                    message = value;
                } else if (_cmd_value(argv[i], "owner") == NULL) {
                    return SL_BAD_COMMAND;
                }
            }

            if (seen != ALL_DSTAR_FIELDS_SEEN
                || !sched_waveform_configureDStar(
                       slc,
                       own_call1,
                       own_call2,
                       companion_call,
                       departure_rptr,
                       destination_rptr,
                       message)) {
                return SL_BAD_COMMAND;
            }
            status_processor_setControlledSlice(
                slc, owner_valid, owner);
            return SUCCESS;
        }
        else if (strcmp(argv[2], "status") == 0 )
        {
            sched_waveform_sendStatus(slc);
        }
        else if (strcmp(argv[2], "tx_select") == 0)
        {
            if ((argc != 5 && argc != 6)
                || (strcmp(argv[4], "0") != 0 && strcmp(argv[4], "1") != 0))
            {
                return SL_BAD_COMMAND;
            }

            BOOL owner_valid = FALSE;
            uint32 owner = 0U;
            if (!_cmd_owner(argc, argv, 5, &owner_valid, &owner)
                || (argc == 6 && !owner_valid)) {
                return SL_BAD_COMMAND;
            }
            status_processor_setControlledSlice(
                slc, owner_valid, owner);

            const BOOL mode_active = strcmp(argv[4], "1") == 0;
            if (strcmp(argv[3], "none") == 0)
            {
                status_processor_setAuthoritativeTxSelection(
                    FALSE, 0U, FALSE);
                return SUCCESS;
            }

            uint32 selected = 0U;
            if (!_cmd_uint32(argv[3], &selected))
            {
                return SL_BAD_COMMAND;
            }
            status_processor_setAuthoritativeTxSelection(
                TRUE, selected, mode_active);
        }
    }

    return SUCCESS;
}
