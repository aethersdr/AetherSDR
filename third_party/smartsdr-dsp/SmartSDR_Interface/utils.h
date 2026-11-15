///*    \file utils.h
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

#ifndef UTILS_H_
#define UTILS_H_

#include "common.h"

void lock_printf_init(void);
void output(const char *fmt,...);
void tsAdd(struct timespec* time1, struct timespec time2);
float tsfSubtract(struct timespec time1, struct timespec time2);
uint32 usSince(struct timespec time);
uint32 msSince(struct timespec time);
uint32 getIP(char* text);
void lock_malloc_init(void);
void* safe_malloc(size_t size);
void safe_free(void* ptr);
void printIP(uint32 ip);
void charReplace( char * string, char oldChar, char newChar );

#endif /* UTILS_H_ */
