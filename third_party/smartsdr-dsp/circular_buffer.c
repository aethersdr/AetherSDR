/* *****************************************************************************
 *	circular_float_buffer.c											2014 AUG 20
 *
 *	General Purpose Circular Buffer Function Set
 *
 *  Created on: Aug 21, 2014
 *      Author: Graham / KE9H
 *
 * *****************************************************************************
 *
 *	Copyright (C) 2014 FlexRadio Systems.
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *	GNU General Public License for more details.
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *******************************************************************************
 *
 *	Example usage:
 *
 *	Circular buffer object, declared in circular_buffer.h
 *
 * 		typedef struct {
 *			unsigned int  size;		// Maximum number of elements + 1
 *			unsigned int  start;	// Index of oldest element
 *			unsigned int  end;		// Index at which to write new element
 *			unsigned char *elems;	// Vector of elements
 *		} circular_buffer, *Circular_Buffer;
 *
 *
 *	Circular Buffer Declaration
 *
 *		unsigned char CL_Buff[2048];		// Example: Command Line Buffer
 *		Note: Make sure declaration matches data size
 *
 *		circular_buffer CommandLine_cb;
 *
 *		Circular_Buffer CL_cb = &CommandLine_cb;
 *
 *
 *	Initialize circular buffer for Command Line buffer
 *	// Includes one empty element
 *		CL_cb->size	 = 2048;		// size = no.elements in array
 *		CL_cb->start = 0;
 *		CL_cb->end	 = 0;
 *		CL_cb->elems = CL_Buff;
 *
 *	Call Read or Write function, appropriate to the data size
 *
 **************************************************************************** */


#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "circular_buffer.h"
#include "utils.h"



/* *****************************************************************************
 *	BOOL cfbIsFull(Circular_Float_Buffer cb)
 *		Test to check if the buffer is FULL
 *		Returns TRUE if full
 */

bool cfbIsFull(Circular_Float_Buffer cb)
{
	return ((cb->end + 1) % cb->size == cb->start);
}


/* *****************************************************************************
 *	BOOL cfbIsEmpty(Circular_Float_Buffer cb)
 *		Test to check if the buffer is EMPTY
 *		Returns TRUE if empty
 */

bool cfbIsEmpty(Circular_Float_Buffer cb)
{
	return cb->end == cb->start;
}


/* *****************************************************************************
 *	void cbWriteChar(Circular_Buffer cb, unsigned char temp)
 *		Write an element, overwriting oldest element if buffer is full.
 *		App can choose to avoid the overwrite by checking cbIsFull().
 */

void cbWriteFloat(Circular_Float_Buffer cb, float sample)
{
	cb->elems[cb->end] = sample;
	cb->end = (cb->end + 1) % cb->size;
	if (cb->end == cb->start) {
		cb->start = (cb->start + 1) % cb->size;		/* full, overwrite */
		output(ANSI_RED "Overwrite! in Circular Float Buffer - Name %s\n"
		        "Size %d Start %d End %d\n "ANSI_WHITE, cb->name, cb->size, cb->start, cb->end);

	}
}


/* *****************************************************************************
 *	unsigned char cbReadChar(CircularBuffer *cb)
 *		Returns oldest element.
 *		Calling function must ensure [cbIsEmpty() != TRUE], first.
 */

float cbReadFloat(Circular_Float_Buffer cb)
{
	float temp;

	temp = cb->elems[cb->start];
	cb->start = (cb->start + 1) % cb->size;
	return temp;
}


/* *****************************************************************************
 *	void zero_cfb(Circular_Float_Buffer cb)
 *
 *		Empties circular buffer
 */

void zero_cfb(Circular_Float_Buffer cb)
{
	cb->start = 0;
	cb->end	 = 0;
}


/* *****************************************************************************
 *	int cfbContains(Circular_Float_Buffer cb)
 *
 *		Returns the number of samples in the circular buffer
 *
 */

int cfbContains(Circular_Float_Buffer cb)
{
	int contains;

	contains = (cb->end - cb->start);
	if (contains < 0)
		contains += cb->size;

	return contains;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  SHORT BUFFER ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


/* *****************************************************************************
 *	BOOL cfbIsFull(Circular_Float_Buffer cb)
 *		Test to check if the buffer is FULL
 *		Returns TRUE if full
 */

bool csbIsFull(Circular_Short_Buffer cb)
{
	return ((cb->end + 1) % cb->size == cb->start);
}


/* *****************************************************************************
 *	BOOL csbIsEmpty(Circular_Short_Buffer cb)
 *		Test to check if the buffer is EMPTY
 *		Returns TRUE if empty
 */

bool csbIsEmpty(Circular_Short_Buffer cb)
{
	return cb->end == cb->start;
}


/* *****************************************************************************
 *	void cbWriteChar(Circular_Buffer cb, unsigned char temp)
 *		Write an element, overwriting oldest element if buffer is full.
 *		App can choose to avoid the overwrite by checking cbIsFull().
 */

void cbWriteShort(Circular_Short_Buffer cb, short sample)
{
	cb->elems[cb->end] = sample;
	cb->end = (cb->end + 1) % cb->size;
	if (cb->end == cb->start) {
		cb->start = (cb->start + 1) % cb->size;		/* full, overwrite */
		output(ANSI_RED "Overwrite! in Circular Short Buffer - Name %s\n" ANSI_WHITE, cb->name);
	}
}


/* *****************************************************************************
 *	unsigned char cbReadShort(Circular_Short_Buffer *cb)
 *		Returns oldest element.
 *		Calling function must ensure [csbIsEmpty() != TRUE], first.
 */

short cbReadShort(Circular_Short_Buffer cb)
{
	short temp;

	temp = cb->elems[cb->start];
	cb->start = (cb->start + 1) % cb->size;
	return temp;
}


/* *****************************************************************************
 *	void zero_cfb(Circular_Short_Buffer cb)
 *
 *		Empties circular buffer
 */

void zero_csb(Circular_Short_Buffer cb)
{
	cb->start = 0;
	cb->end	 = 0;
}


/* *****************************************************************************
 *	int csbContains(Circular_Short_Buffer cb)
 *
 *		Returns the number of samples in the circular buffer
 *
 */

int csbContains(Circular_Short_Buffer cb)
{
	int contains;

	contains = (cb->end - cb->start);
	if (contains < 0)
		contains += cb->size;

		return contains;
}


// EoF =====-----
