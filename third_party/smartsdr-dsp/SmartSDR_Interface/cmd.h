/* *****************************************************************************
 *	cmd.h															2014 AUG 31
 *
 *		Header file for cmd_engine.c and cmd_basics.c
 *
 *		date March 30, 2012
 * 		author Stephen Hicks, N5AC
 *
 * *****************************************************************************
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


#ifndef CMD_H_
#define CMD_H_

#include "datatypes.h"


#define ACCESS_GUEST			0x0
#define ACCESS_SERIAL 			0x1
#define ACCESS_TCP 				0x2
#define ACCESS_SERIAL_TCP 		0x3
#define SVC_SER					1
#define SVC_TCP					2
#define SVC_BOT					3

#define MAX_ARGC 				16
#define MAX_ARGC_STATUS         100

// #define PROMPT "\033[92mSmartSDR> \033[m"

extern char* CMD_UNRECOGNIZED;
extern char* EEPROM_BAD_BYTE;
extern char* EEPROM_TOO_LARGE;
extern char* EEPROM_USAGE;


/* ------------------------------------------------------------------------ *
 *     						   	Prototypes                                 	*
 * ------------------------------------------------------------------------ */

unsigned int command (void);
void tokenize (char *line, int *pargc, char **argv, int max_arguments);
//void process_command  (int fd, char line[]);
uint32 process_command(char* command_txt);

uint32 cmd_banner();
uint32 cmd_cls(int requester_fd, int argc,char **argv);
uint32 cmd_date(int requester_fd, int argc,char **argv);
uint32 cmd_exit(int requester_fd, int argc,char **argv);
uint32 cmd_help(int requester_fd, int argc, char **argv);
uint32 cmd_slice(int requester_fd, int argc,char **argv);
uint32 cmd_time(int requester_fd, int argc,char **argv);
// uint32 cmd_register(int requester_fd, int argc,char **argv);
uint32 cmd_undefined(int requester_fd, int argc,char **argv);

void get_line(char* line, int maxlen);

char getch(void);	// Read 1 character
char getche(void);	// Read 1 character with echo


#endif /* CMD_H_ */
