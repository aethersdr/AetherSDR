///*    \file utils.c
// *    \brief Utility Functions
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

#include <unistd.h> // for usleep
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>
#include <stdarg.h>
#include <semaphore.h>

#include "common.h"

static pthread_mutex_t _printf_mutex;
static pthread_mutex_t _malloc_mutex;

void lock_printf_init(void)
{
    pthread_mutex_init(&_printf_mutex, NULL);
}

static void _output_stdio(const char *fmt, va_list args)
{
    pthread_mutex_lock(&_printf_mutex);
    vprintf(fmt, args);
    fflush(stdout);
    pthread_mutex_unlock(&_printf_mutex);
}

void output(const char *fmt,...)
{
    va_list args;

    va_start(args, fmt);
    _output_stdio(fmt, args);
    va_end(args);
}

int32 tsSubtract(struct timespec time1, struct timespec time2)
{
    int64 result;
    result = (time1.tv_sec - time2.tv_sec) * 1000ll;
    result += (time1.tv_nsec - time2.tv_nsec) / 1000000ll;
    return (int32)result;
}

float tsfSubtract(struct timespec time1, struct timespec time2)
{
    float result;
    result = (time1.tv_sec - time2.tv_sec) * 1000.0;
    result += (time1.tv_nsec - time2.tv_nsec) / 1000000.0;
    return result;
}


//! get time since a certain time in microseconds
uint32 usSince(struct timespec time)
{
    struct timespec delay;
    clock_gettime(CLOCK_MONOTONIC, &delay);
    uint32 diff_us = (uint32)(tsfSubtract(delay, time) * 1000.0);
    return diff_us;
}

uint32 msSince(struct timespec time)
{
    struct timespec delay;
    clock_gettime(CLOCK_MONOTONIC, &delay);
    uint32 diff_ms = (uint32)(tsSubtract(delay, time));
    return diff_ms;
}

uint32 getIP(char* text)
{
    uint32 ip;
    int a,b,c,d;
    // get IP address
    sscanf(text, "%d.%d.%d.%d", &a,&b,&c,&d);
    ip = (a << 24) + (b << 16) + (c << 8) + d;
    return ip;
}

void lock_malloc_init(void)
{
    pthread_mutex_init(&_malloc_mutex, NULL);
}

// thread-safe malloc
void* safe_malloc(size_t size)
{
    void* pmem;
    pthread_mutex_lock(&_malloc_mutex);
    pmem = malloc(size);
    pthread_mutex_unlock(&_malloc_mutex);
    return pmem;
}

// thread-safe free
void safe_free(void* ptr)
{
    pthread_mutex_lock(&_malloc_mutex);
    if (ptr != NULL)
    {
        free(ptr);
    }
    else
    {
        output(ANSI_RED "Utils: attempt to free a NULL pointer\n");
    }
    pthread_mutex_unlock(&_malloc_mutex);
}

void printIP(uint32 ip)
{
	output("%d.%d.%d.%d\n",((ip>>24)& 0xFF),((ip>>16)& 0xFF),((ip>>8)& 0xFF),(ip & 0xFF));
}

void charReplace( char * string, char oldChar, char newChar )
{
	if ( string == NULL ) {
		output("Null string passed to charReplace\n");
		return ;
	}

	while (*string != 0 ) {
		if ( *string == oldChar )
			*string = newChar;

		string++;
	}

}
