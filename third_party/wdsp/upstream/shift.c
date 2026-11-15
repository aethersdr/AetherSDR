/*  shift.c

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

#include "comm.h"

void calc_shift (SHIFT a)
{
	a->delta = TWOPI * a->shift / a->rate;
	a->cos_delta = cos (a->delta);
	a->sin_delta = sin (a->delta);
}

SHIFT create_shift (int run, int size, double* in, double* out, int rate, double fshift)
{
	SHIFT a = (SHIFT) malloc0 (sizeof (shift));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = (double)rate;
	a->shift = fshift;
	a->phase = 0.0;
	calc_shift (a);
	return a;
}

void destroy_shift (SHIFT a)
{
	_aligned_free (a);
}

void flush_shift (SHIFT a)
{
	a->phase = 0.0;
}

void xshift (SHIFT a)
{
	if (a->run)
	{
		int i;
		double I1, Q1, t1, t2;
		double cos_phase = cos (a->phase);
		double sin_phase = sin (a->phase);
		for (i = 0; i < a->size; i++)
		{
			I1 = a->in[2 * i + 0];
			Q1 = a->in[2 * i + 1];
			a->out[2 * i + 0] = I1 * cos_phase - Q1 * sin_phase;
			a->out[2 * i + 1] = I1 * sin_phase + Q1 * cos_phase;
			t1 = cos_phase;
			t2 = sin_phase;
			cos_phase = t1 * a->cos_delta - t2 * a->sin_delta;
			sin_phase = t1 * a->sin_delta + t2 * a->cos_delta;
			a->phase += a->delta;
			if (a->phase >= TWOPI) a->phase -= TWOPI;
			if (a->phase <   0.0 ) a->phase += TWOPI;
		}
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_shift(SHIFT a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_shift (SHIFT a, int rate)
{
	a->rate = rate;
	a->phase = 0.0;
	calc_shift(a);
}

void setSize_shift (SHIFT a, int size)
{
	a->size = size;
	flush_shift (a);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXAShiftRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].shift.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAShiftFreq (int channel, double fshift)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].shift.p->shift = fshift;
	calc_shift (rxa[channel].shift.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}