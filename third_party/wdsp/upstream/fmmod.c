/*  fmmod.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2023 Warren Pratt, NR0V

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

void calc_fmmod (FMMOD a)
{
	// ctcss gen
	a->tscale = 1.0 / (1.0 + a->ctcss_level);
	a->tphase = 0.0;
	a->tdelta = TWOPI * a->ctcss_freq / a->samplerate;
	// mod
	a->sphase = 0.0;
	a->sdelta = TWOPI * a->deviation / a->samplerate;
	// bandpass
	a->bp_fc = a->deviation + a->f_high;
}

FMMOD create_fmmod (int run, int size, double* in, double* out, int rate, double dev, double f_low, double f_high, 
	int ctcss_run, double ctcss_level, double ctcss_freq, int bp_run, int nc, int mp)
{
	FMMOD a = (FMMOD) malloc0 (sizeof (fmmod));
	double* impulse;
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->samplerate = (double)rate;
	a->deviation = dev;
	a->f_low = f_low;
	a->f_high = f_high;
	a->ctcss_run = ctcss_run;
	a->ctcss_level = ctcss_level;
	a->ctcss_freq = ctcss_freq;
	a->bp_run = bp_run;
	a->nc = nc;
	a->mp = mp;
	calc_fmmod (a);
	impulse = fir_bandpass(a->nc, -a->bp_fc, +a->bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
	a->p = create_fircore (a->size, a->out, a->out, a->nc, a->mp, 8, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_fmmod (FMMOD a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_fmmod (FMMOD a)
{
	a->tphase = 0.0;
	a->sphase = 0.0;
}

void xfmmod (FMMOD a)
{
	int i;
	double dp, magdp, peak;
	if (a->run)
	{
		peak = 0.0;
		for (i = 0; i < a->size; i++)
		{
			if (a->ctcss_run)
			{
				a->tphase += a->tdelta;
				if (a->tphase >= TWOPI) a->tphase -= TWOPI;
				a->out[2 * i + 0] = a->tscale * (a->in[2 * i + 0] + a->ctcss_level * cos (a->tphase));
			}
			dp = a->out[2 * i + 0] * a->sdelta;
			a->sphase += dp;
			if (a->sphase >= TWOPI) a->sphase -= TWOPI;
			if (a->sphase <   0.0 ) a->sphase += TWOPI;
			a->out[2 * i + 0] = 0.7071 * cos (a->sphase);
			a->out[2 * i + 1] = 0.7071 * sin (a->sphase);
			if ((magdp = dp) < 0.0) magdp = - magdp;
			if (magdp > peak) peak = magdp;
		}
		//print_deviation ("peakdev.txt", peak, a->samplerate);
		if (a->bp_run)
			xfircore (a->p);
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_fmmod (FMMOD a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	calc_fmmod (a);
	setBuffers_fircore (a->p, a->out, a->out);
}

void setSamplerate_fmmod (FMMOD a, int rate)
{
	double* impulse;
	a->samplerate = rate;
	calc_fmmod (a);
	impulse = fir_bandpass(a->nc, -a->bp_fc, +a->bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void setSize_fmmod (FMMOD a, int size)
{
	double* impulse;
	a->size = size;
	calc_fmmod (a);
	setSize_fircore (a->p, a->size);
	impulse = fir_bandpass(a->nc, -a->bp_fc, +a->bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetTXAFMDeviation (int channel, double deviation)
{
	FMMOD a = txa[channel].fmmod.p;
	double bp_fc = a->f_high + deviation;
	double* impulse = fir_bandpass (a->nc, -bp_fc, +bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
	setImpulse_fircore (a->p, impulse, 0);
	_aligned_free (impulse);
	EnterCriticalSection (&ch[channel].csDSP);
	a->deviation = deviation;
	// mod
	a->sphase = 0.0;
	a->sdelta = TWOPI * a->deviation / a->samplerate;
	// bandpass
	a->bp_fc = bp_fc;
	setUpdate_fircore (a->p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXACTCSSFreq (int channel, double freq)
{
	FMMOD a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].fmmod.p;
	a->ctcss_freq = freq;
	a->tphase = 0.0;
	a->tdelta = TWOPI * a->ctcss_freq / a->samplerate;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXACTCSSRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].fmmod.p->ctcss_run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAFMNC (int channel, int nc)
{
	FMMOD a;
	double* impulse;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].fmmod.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		impulse = fir_bandpass (a->nc, -a->bp_fc, +a->bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT 
void SetTXAFMMP (int channel, int mp)
{
	FMMOD a;
	a = txa[channel].fmmod.p;
	if (a->mp != mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
}

PORT
void SetTXAFMAFFreqs (int channel, double low, double high)
{
	FMMOD a;
	double* impulse;
	EnterCriticalSection(&ch[channel].csDSP);
	a = txa[channel].fmmod.p;
	if (a->f_low != low || a->f_high != high)
	{
		a->f_low = low;
		a->f_high = high;
		a->bp_fc = a->deviation + a->f_high;
		impulse = fir_bandpass (a->nc, -a->bp_fc, +a->bp_fc, a->samplerate, 0, 1, 1.0 / (2 * a->size));
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
	LeaveCriticalSection(&ch[channel].csDSP);
}