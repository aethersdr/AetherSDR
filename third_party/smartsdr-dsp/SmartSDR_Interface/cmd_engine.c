/* *****************************************************************************
 *	cmd_engine.c
 *
 *   \brief Command Engine - Command processing
 *
 *
 * 	\author Terry, AB5K
 * 	\author Stephen Hicks, N5AC
 * 	\date   23-AUG-2011
 *
 *	Instructions for adding a new command:
 *	1. Add a reference in the "command_defs commands[]" array below
 *	2. If help is needed, add a reference to lpszCommandDescriptionBasic[] in the cmd_basics.c file.
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


#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#ifdef _WIN32
#include <conio.h>
#else
#include <sys/termios.h>
#endif

#include "../common.h"
#include "datatypes.h"

#include "cmd.h"
#include "main.h"

#ifndef _WIN32
static struct termios old, new;		// For Terminal IO
#endif

//#include "drv_bq32000.h"
//#include "messages.h"
//#include "client_manager.h"
//#include "radio.h"

char *CMD_UNRECOGNIZED	= "Unrecognized command \"%s\".\r\n";

// Command Function Pointer called by the interpreter.
typedef uint32 handler(int, int, char**);

static char PrevLine[80];

typedef struct _command_def
{
	const char* commandName;
	handler *pFunction;
} command_def;


static command_def commands[] =
{
        {   0,                      cmd_undefined               },  // Space Holder
        {   "b",                    cmd_banner                  },  // Display Banner
        {   "banner",               cmd_banner                  },  // Display Banner
        {   "cls",                  cmd_cls                     },  // Clear Screen
        {   "date",                 cmd_date                    },  // Print the date/time
        {   "exit",                 cmd_exit                    },  // Exit the program
        {   "help",                 cmd_help                    },  // Help Menu
        {   "quit",                 cmd_exit                    },  // Exit the program
        {   "slice",                cmd_slice                   },  // Handle slice changes
        {   "time",                 cmd_time                    },  // Print the time
        {   "?",                    cmd_help                    },  // Display Help
        {   0,                      cmd_undefined               },  // Undefined - must be last in the list

};

//long execute_command(int requester_fd, int cmd_num, int argc, char *argv);

// #################################################################
// ##
// ##  Command Interpreter
// ##
// #################################################################
unsigned int command(void)
{
	char line[512];
	char *pLine = line;
	get_line(pLine, 512 * sizeof(char));

	process_command(line);

	return (1);
}

uint32 process_command(char* command_txt)
{
	uint32 cmd_ret = SUCCESS;
	int cmd_num;
	int argc;
	char *argv[MAX_ARGC + 1];		//Add one extra so we can null terminate the array

	output("CMD Received = '%s'\n", command_txt);

	tokenize(command_txt, &argc, argv, MAX_ARGC);

	if (argc > 0)
	{
		cmd_num = 1;

		while (cmd_num > 0)
		{
			if ((commands[cmd_num].commandName == 0) || (strcmp(argv[0], commands[cmd_num].commandName) == 0))
			{
			    //Execute the requested command
                cmd_ret = commands[cmd_num].pFunction(1, argc, argv);
				cmd_num = 0;
			}
			else
			{
				cmd_num++;
			}
		}
	}
	return cmd_ret;
}


// #################################################################
// ##
// ##  void tokenize(char*, int*, char**, max_arguments);
// ##
// ##  Breaks a single character string into an array of tokens.
// #################################################################

void tokenize(
	char*	line,            // Input String
	int*	pargc,           // Number of arguments
	char**	argv,            // Array of strings holding tokens
	int     max_arguments    // Maximum Tokens allowed
)
{
	BOOL inside_string = FALSE;
	BOOL inside_token = FALSE;
	char* readp;

	*pargc = 0;

		// Read through the entire string searching for tokens
	for (readp = line; *readp; readp++)
	{
			// Search for start of token
		if (!inside_token)
		{
			// Ignore white spaces
			if ((*readp == ' ') || (*readp == '\t'))
			{
				;
			}
			// Start of a new token
			else
			{
				if (*readp == '\"')
				{
					inside_string = TRUE;
				}
				else
				{
		inside_token = TRUE;
				argv[*pargc] = readp;
					(*pargc)++;

					if(*pargc > max_arguments)
						break;
				}
			}
		}

		// We are inside the token
		else
		{ // inside token

			// Found the end of the token
			if ( (!inside_string && ((*readp == ' ') || (*readp == '\t'))) |
			     (inside_string && (*readp == '\"'))
			   )
			{
					inside_string = FALSE;
				inside_token = FALSE;
				*readp = 0;
			}
		}
	}

	// End of input line terminates a token
	if (inside_token)
	{
		*readp = 0;
		readp++;
	}

	if(*pargc > max_arguments)
	{
		*pargc = max_arguments;
	}
	argv[*pargc] = 0; // Null-terminate just to be nice
}



// #################################################################
// ##
// ##  void command_get_line(char *, int)
// ##
// #################################################################
void get_line(char * line, int maxlen)
{
	char *pLine = line;
	char *pPrevLine = PrevLine;
	char   c = 0;

	*pLine = 0;
	for (;;) {
		c = aether_getch();

		// Escape Sequences
		if (c == '\033') {
			while (pLine > line) {

				printf("\b \b");
				pLine--;
				*pLine = 0;
			}
			c = aether_getch();

			if (c == '[') {
				c = aether_getch();

				if (c == 'A') {
					// Restore previous command
					pLine = line;
					pPrevLine = PrevLine;
					while(*pPrevLine) {

						*pLine = *pPrevLine;
						pLine++;
						pPrevLine++;
					}
					*pLine = 0;
					printf("%s",line);
				}
			}
		}

		else if ((c == '\n') || (c == '\r')) {
			printf("\r\n");
			break;
		}

		// Check for backspace or delete key.
		else if ((c == '\b') || (c == 0x7F)) {
			if (pLine > line) {
				printf("\b \b");
				pLine--;
				*pLine = 0;
			}
		}

		// Check for escape key or control-U.
		else if (c == 0x15) {
			while (pLine > line) {
				printf("\b \b");
				pLine--;
				*pLine = 0;
			}
		}

		else if (c > 0) {
			printf("%c",c);
			*pLine = c;
			pLine++;
			*pLine = 0;
		}
	}

	*pLine = 0;

	pLine = line;
	pPrevLine = PrevLine;
	while(*pLine) {
		*pPrevLine = *pLine;
		pLine++;
		pPrevLine++;
	}
	*pPrevLine = 0;
}


/* *****************************************************************************
 *  getch() and getche() functionality for UNIX,
 * 	based on termios (terminal handling functions)
 *
 *  This code snippet was written by Wesley Stessens (wesley@ubuntu.com)
 *  It is released in the Public Domain.
 */

/* Initialize new terminal i/o settings */
#ifdef _WIN32
void initTermios(int echo) {
    (void)echo;
}

void resetTermios(void) {
}

char aether_getch_impl(int echo) {
    return (char)(echo ? _getche() : _getch());
}
#else
void initTermios(int echo) {
    int n = tcgetattr(0, &old); /* grab old terminal i/o settings */
	if (n == -1) return;

    new = old; /* make new settings same as old settings */
    new.c_lflag &= ~ICANON; /* disable buffered i/o */
    new.c_lflag &= echo ? ECHO : ~ECHO; /* set echo mode */
    tcsetattr(0, TCSANOW, &new); /* use these new terminal i/o settings now */
}

/* Restore old terminal i/o settings */
void resetTermios(void) {
	tcsetattr(0, TCSANOW, &old);
}

/* Read 1 character - echo defines echo mode */
char aether_getch_impl(int echo) {
    char ch;
    initTermios(echo);
    ch = (char)getchar();
    resetTermios();
    return ch;
}
#endif

/* Read 1 character without echo */
char aether_getch(void) {
    return aether_getch_impl(0);
}

/* Read 1 character with echo */
char aether_getche(void) {
    return aether_getch_impl(1);
}
