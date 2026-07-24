/*  cblock.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016 Warren Pratt, NR0V

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

void calc_cbl (CBL a)
{
	a->prevIin  = 0.0;
	a->prevQin  = 0.0;
	a->prevIout = 0.0;
	a->prevQout = 0.0;
	a->mtau = exp(-1.0 / (a->sample_rate * a->tau));
}

CBL create_cbl
	(
	int run,
	int buff_size,
	double *in_buff,
	double *out_buff,
	int mode,
	int sample_rate,
	double tau
	)
{
	CBL a = (CBL) malloc0 (sizeof(cbl));
	a->run = run;
	a->buff_size = buff_size;
	a->in_buff = in_buff;
	a->out_buff = out_buff;
	a->mode = mode;
	a->sample_rate = (double)sample_rate;
	a->tau = tau;
	calc_cbl (a);
	return a;
}

void destroy_cbl(CBL a)
{
	_aligned_free (a);
}

void flush_cbl (CBL a)
{
	a->prevIin  = 0.0;
	a->prevQin  = 0.0;
	a->prevIout = 0.0;
	a->prevQout = 0.0;
}

void xcbl (CBL a)
{
	if (a->run)
	{
		int i;
		double tempI, tempQ;
		for (i = 0; i < a->buff_size; i++)
		{
			tempI  = a->in_buff[2 * i + 0];
			tempQ  = a->in_buff[2 * i + 1];
			a->out_buff[2 * i + 0] = a->in_buff[2 * i + 0] - a->prevIin + a->mtau * a->prevIout;
			a->out_buff[2 * i + 1] = a->in_buff[2 * i + 1] - a->prevQin + a->mtau * a->prevQout;
			a->prevIin  = tempI;
			a->prevQin  = tempQ;
			if (fabs(a->prevIout = a->out_buff[2 * i + 0]) < 1.0e-100) a->prevIout = 0.0;
			if (fabs(a->prevQout = a->out_buff[2 * i + 1]) < 1.0e-100) a->prevQout = 0.0;
		}
	}
	else if (a->in_buff != a->out_buff)
		memcpy (a->out_buff, a->in_buff, a->buff_size * sizeof (complex));
}

void setBuffers_cbl (CBL a, double* in, double* out)
{
	a->in_buff = in;
	a->out_buff = out;
}

void setSamplerate_cbl (CBL a, int rate)
{
	a->sample_rate = rate;
	calc_cbl (a);
}

void setSize_cbl (CBL a, int size)
{
	a->buff_size = size;
	flush_cbl (a);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT void
SetRXACBLRun(int channel, int setit)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].cbl.p->run = setit;
	LeaveCriticalSection (&ch[channel].csDSP);
}