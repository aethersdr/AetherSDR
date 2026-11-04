/*  nnrbypass.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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

warren@pratt.one

*/

#include <malloc.h>
#include <string.h>
#include <Windows.h>
typedef double complex[2];
#include "fftw3.h"
#include "fir.h"
#include "firmin.h"

typedef struct _nnrbypass
{
	int run;					// 0 - bypass is OFF; 1 - bypass is ON
	int size;					// input/output buffer size
	int nc_default;				// default number of impulse response coeffs
	int nc;						// current number of impulse response coeffs
	double* in;					// pointer to input buffer
	double* out;				// pointer to output buffer
	double f_low;			    // filter lower frequency (Hz)
	double f_high;			    // filter higher frequency (Hz)
	int samplerate;			    // sample_rate (samples/sec)
	double gain;				// gain to be applied to filter output
	int wintype;				// 0 -> 4-term Blackman-Harris
	int mp;						// 1 -> Minimum Phase
	double scale;				// internal filter scale factor based upon gain
	double* buff;				// buffer to hold input signal
	FIRCORE p;
} nnrbypass, *NNRBYPASS;

static void* zmalloc (int size)
{
	const int alignment = 16;
	void* p = _aligned_malloc (size, alignment);
	if (p != 0) memset (p, 0, size);
	return p;
}

NNRBYPASS create_nnrbypass (int run, int size, int nc_default, double* in, double* out,
	double f_low, double f_high, int samplerate, double gain)
{
	NNRBYPASS b = (NNRBYPASS)zmalloc (sizeof(nnrbypass));
	b->run = run;
	b->size = size;
	b->nc_default = nc_default;
	b->in = in;
	b->out = out;
	b->f_low = f_low;
	b->f_high = f_high;
	b->samplerate = samplerate;
	b->gain = gain;
	b->wintype = 0;
	b->mp = 1;
	b->scale = b->gain / (double)(2 * b->size);
	b->buff = (double*)zmalloc (b->size * sizeof(complex));
	if (b->size > b->nc_default) b->nc = b->size;
	else                         b->nc = b->nc_default;
	double* impulse = fir_bandpass (b->nc, b->f_low, b->f_high, b->samplerate, b->wintype, 1, b->scale);
	b->p = create_fircore (b->size, b->buff, b->out, b->nc, b->mp, 16, impulse);
	_aligned_free (impulse);
	return b;
}

void destroy_nnrbypass (NNRBYPASS b)
{
	destroy_fircore (b->p);
	_aligned_free (b->buff);
	_aligned_free (b);
}

void flush_nnrbypass (NNRBYPASS b)
{
	memset (b->buff, 0, b->size * sizeof(complex));
	flush_fircore (b->p);
}

void xnnrbypin (NNRBYPASS b)
{
	if (b->run)
		memcpy (b->buff, b->in, b->size * sizeof(complex));
}

void xnnrbypout (NNRBYPASS b)
{
	if (b->run)
		xfircore (b->p);
}

void setBuffers_nnrbypass (NNRBYPASS b, double* in, double* out)
{
	b->in = in;
	b->out = out;
	setBuffers_fircore (b->p, b->buff, b->out);
}

void setSamplerate_nnrbypass (NNRBYPASS b, int rate)
{
	b->samplerate = rate;
	double* impulse = fir_bandpass (b->nc, b->f_low, b->f_high, b->samplerate, b->wintype, 1, b->scale);
	setImpulse_fircore (b->p, impulse, 1);
	_aligned_free (impulse);
}

void setSize_nnrbypass (NNRBYPASS b, int size)
{
	b->size = size;
	if (b->size > b->nc_default) b->nc = b->size;
	else                         b->nc = b->nc_default;
	_aligned_free (b->buff);
	b->buff = (double*)zmalloc (b->size * sizeof(complex));
	b->scale = b->gain / (double)(2 * b->size);
	destroy_fircore (b->p);
	double* impulse = fir_bandpass (b->nc, b->f_low, b->f_high, b->samplerate, b->wintype, 1, b->scale);
	b->p = create_fircore (b->size, b->buff, b->out, b->nc, b->mp, 16, impulse);
	_aligned_free (impulse);
}

void setRun_nnrbypass (NNRBYPASS b, int run, double f_low, double f_high)
{
	int was = b->run;
	if (run && (b->f_low != f_low || b->f_high != f_high))
	{
		b->f_low  = f_low;
		b->f_high = f_high;
		double* impulse = fir_bandpass (b->nc, b->f_low, b->f_high, b->samplerate, b->wintype, 1, b->scale);
		setImpulse_fircore (b->p, impulse, 1);
		_aligned_free (impulse);
	}
	// The fircore still holds whatever it was filtering when it last ran -
	//    possibly long ago - so clear it rather than play that out.
	if (run && !was) flush_nnrbypass (b);
	b->run = run;
}

int getRun_nnrbypass (NNRBYPASS b)
{
	return b->run;
}

// Width of the bypass passband, Hz.  When the bypass runs this is what the
//    AGC integrates over - see the threshold functions in wcpAGC.c.
double getBandwidth_nnrbypass (NNRBYPASS b)
{
	return b->f_high - b->f_low;
}
