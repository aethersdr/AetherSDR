/*  slew.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#ifndef _slew_h
#define _slew_h

typedef struct _uslew
{
	int channel;
	volatile long *ch_upslew;
	int size;
	double* in;
	double* out;
	double rate;
	double tdelay;
	double tupslew;
	int runmode;
	int state;
	int count;
	int ndelup;
	int ntup;
	double* cup;
} uslew, *USLEW;

extern USLEW create_uslew (int channel, volatile long *ch_upslew, int size, double* in, double* out, double rate, double tdelay, double tupslew);

extern void destroy_uslew (USLEW a);

extern void flush_uslew (USLEW a);

extern void xuslew (USLEW a);

extern void setBuffers_uslew (USLEW a, double* in, double* out);

extern void setSamplerate_uslew (USLEW a, int rate);

extern void setSize_uslew (USLEW a, int size);

#endif