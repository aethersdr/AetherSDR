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

#include "common.h"
#include "main.h"
#include "cmd.h"

#include "main.h"
#include "sched_waveform.h"


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
        errno = 0;
        slc = strtoul(argv[1], NULL, 0);
        if(errno)
        {
            output(ANSI_RED "Unable to parse slice number (%s)\n", argv[1]);
            return SL_BAD_COMMAND;
        }

        if(strncmp(argv[2], "set", strlen("set")) == 0)
        {
            // charReplace(new_string, (char) 0x7F, ' ');

            int i = 0;
            for ( i = 3 ; i < argc ; i++ ) {

                if ( strncmp(argv[i], "destination_rptr", strlen("destination_rptr") )  == 0 ) {

                    char * rptr = argv[i] + strlen("destination_rptr") + 1;
                    sched_waveform_setDestinationRptr(slc, rptr);

                } else if ( strncmp(argv[i], "departure_rptr", strlen("departure_rptr") )  == 0 ) {

                    char * rptr = argv[i] + strlen("departure_rptr") + 1;
                    sched_waveform_setDepartureRptr(slc, rptr);

                } else if ( strncmp(argv[i], "companion_call", strlen("companion_call") )  == 0 ) {
                    char * call = argv[i] + strlen("companion_call") + 1;
                    sched_waveform_setCompanionCall(slc, call);

                } else if ( strncmp(argv[i], "own_call1", strlen("own_call1") )  == 0 ) {
                    char * call = argv[i] + strlen("own_call1") + 1;
                    sched_waveform_setOwnCall1(slc, call);

                } else if ( strncmp(argv[i], "own_call2", strlen("own_call2") )  == 0 ) {
                    char * call = argv[i] + strlen("own_call2") + 1;
                    sched_waveform_setOwnCall2(slc, call);
                } else if ( strncmp(argv[i], "message", strlen("message")) == 0 ) {
                    char * message = argv[i] + strlen("message") + 1;
                    sched_waveform_setMessage(slc, message);
                }



            }

            return SUCCESS;
        }
        else if (strncmp(argv[2], "status", strlen("status")) == 0 )
        {
            sched_waveform_sendStatus(slc);
        }
    }

    return SUCCESS;
}
