/*  emph.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014, 2016, 2023, 2026 Warren Pratt, NR0V

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

#include "comm.h"

/********************************************************************************************************
*																										*
*								Partitioned Overlap-Save FM Pre-Emphasis								*
*																										*
********************************************************************************************************/

EMPHP create_emphp (int run, int position, int size, int nc, int mp, double* in, double* out, int rate, int ctype, double f_low, double f_high)
{
	EMPHP a = (EMPHP) malloc0 (sizeof (emphp));
	double* impulse;
	a->run = run;
	a->position = position;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->ctype = ctype;
	a->f_low = f_low;
	a->f_high = f_high;
	impulse = fc_impulse (a->nc, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, a->mp, 4, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_emphp (EMPHP a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_emphp (EMPHP a)
{
	flush_fircore (a->p);
}

void xemphp (EMPHP a, int position)
{
	if (a->run && a->position == position)
		xfircore (a->p);
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_emphp (EMPHP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_emphp (EMPHP a, int rate)
{
	double* impulse;
	a->rate = rate;
	impulse = fc_impulse (a->nc, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void setSize_emphp (EMPHP a, int size)
{
	double* impulse;
	a->size = size;
	setSize_fircore (a->p, a->size);
	impulse = fc_impulse (a->nc, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

/********************************************************************************************************
*																										*
*						Partitioned Overlap-Save FM Pre-Emphasis:  TXA Properties						*
*																										*
********************************************************************************************************/

PORT
void SetTXAFMEmphPosition (int channel, int position)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].preemph.p->position = position;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAFMEmphMP (int channel, int mp)
{
	EMPHP a;
	a = txa[channel].preemph.p;
	if (a->mp != mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
}

PORT
void SetTXAFMEmphNC (int channel, int nc)
{
	EMPHP a;
	double* impulse;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].preemph.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		impulse = fc_impulse (a->nc, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAFMPreEmphFreqs (int channel, double low, double high)
{
	EMPHP a;
	double* impulse;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].preemph.p;
	if (a->f_low != low || a->f_high != high)
	{
		a->f_low = low;
		a->f_high = high;
		impulse = fc_impulse (a->nc, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

/********************************************************************************************************
*																										*
*										Overlap-Save FM Pre-Emphasis									*
*																										*
********************************************************************************************************/

void calc_emph (EMPH a)
{
	a->infilt = (double *)malloc0(2 * a->size * sizeof(complex));
	a->product = (double *)malloc0(2 * a->size * sizeof(complex));
	a->mults = fc_mults(a->size, a->f_low, a->f_high, -20.0 * log10(a->f_high / a->f_low), 0.0, a->ctype, a->rate, 1.0 / (2.0 * a->size), 0, 0);
	a->CFor = fftw_plan_dft_1d(2 * a->size, (fftw_complex *)a->infilt, (fftw_complex *)a->product, FFTW_FORWARD, FFTW_PATIENT);
	a->CRev = fftw_plan_dft_1d(2 * a->size, (fftw_complex *)a->product, (fftw_complex *)a->out, FFTW_BACKWARD, FFTW_PATIENT);
}

void decalc_emph (EMPH a)
{
	fftw_destroy_plan(a->CRev);
	fftw_destroy_plan(a->CFor);
	_aligned_free(a->mults);
	_aligned_free(a->product);
	_aligned_free(a->infilt);
}

EMPH create_emph (int run, int position, int size, double* in, double* out, int rate, int ctype, double f_low, double f_high)
{
	EMPH a = (EMPH) malloc0 (sizeof (emph));
	a->run = run;
	a->position = position;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = (double)rate;
	a->ctype = ctype;
	a->f_low = f_low;
	a->f_high = f_high;
	calc_emph (a);
	return a;
}

void destroy_emph (EMPH a)
{
	decalc_emph (a);
	_aligned_free (a);
}

void flush_emph (EMPH a)
{
	memset (a->infilt, 0, 2 * a->size * sizeof (complex));
}

void xemph (EMPH a, int position)
{
	int i;
	double I, Q;
	if (a->run && a->position == position)
	{
		memcpy (&(a->infilt[2 * a->size]), a->in, a->size * sizeof (complex));
		fftw_execute (a->CFor);
		for (i = 0; i < 2 * a->size; i++)
		{
			I = a->product[2 * i + 0];
			Q = a->product[2 * i + 1];
			a->product[2 * i + 0] = I * a->mults[2 * i + 0] - Q * a->mults[2 * i + 1];
			a->product[2 * i + 1] = I * a->mults[2 * i + 1] + Q * a->mults[2 * i + 0];
		}
		fftw_execute (a->CRev);
		memcpy (a->infilt, &(a->infilt[2 * a->size]), a->size * sizeof(complex));
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_emph (EMPH a, double* in, double* out)
{
	decalc_emph (a);
	a->in = in;
	a->out = out;
	calc_emph (a);
}

void setSamplerate_emph (EMPH a, int rate)
{
	decalc_emph (a);
	a->rate = rate;
	calc_emph (a);
}

void setSize_emph (EMPH a, int size)
{
	decalc_emph(a);
	a->size = size;
	calc_emph(a);
}

/********************************************************************************************************
*																										*
*								Overlap-Save FM Pre-Emphasis:  TXA Properties							*
*																										*
********************************************************************************************************/
/*	// Uncomment when needed
PORT
void SetTXAFMEmphPosition (int channel, int position)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].preemph.p->position = position;
	LeaveCriticalSection (&ch[channel].csDSP);
}
*/