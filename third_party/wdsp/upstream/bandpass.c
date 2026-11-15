/*  bandpass.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2017 Warren Pratt, NR0V

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

/********************************************************************************************************
*																										*
*										Overlap-Save Bandpass											*
*																										*
********************************************************************************************************/

void calc_bps (BPS a)
{
	double* impulse;
	a->infilt = (double *)malloc0(2 * a->size * sizeof(complex));
	a->product = (double *)malloc0(2 * a->size * sizeof(complex));
	impulse = fir_bandpass(a->size + 1, a->f_low, a->f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
	a->mults = fftcv_mults(2 * a->size, impulse);
	a->CFor = fftw_plan_dft_1d(2 * a->size, (fftw_complex *)a->infilt, (fftw_complex *)a->product, FFTW_FORWARD, FFTW_PATIENT);
	a->CRev = fftw_plan_dft_1d(2 * a->size, (fftw_complex *)a->product, (fftw_complex *)a->out, FFTW_BACKWARD, FFTW_PATIENT);
	_aligned_free(impulse);
}

void decalc_bps (BPS a)
{
	fftw_destroy_plan(a->CRev);
	fftw_destroy_plan(a->CFor);
	_aligned_free(a->mults);
	_aligned_free(a->product);
	_aligned_free(a->infilt);
}

BPS create_bps (int run, int position, int size, double* in, double* out, 
	double f_low, double f_high, int samplerate, int wintype, double gain)
{
	BPS a = (BPS) malloc0 (sizeof (bps));
	a->run = run;
	a->position = position;
	a->size = size;
	a->samplerate = (double)samplerate;
	a->wintype = wintype;
	a->gain = gain;
	a->in = in;
	a->out = out;
	a->f_low = f_low;
	a->f_high = f_high;
	calc_bps (a);
	return a;
}

void destroy_bps (BPS a)
{
	decalc_bps (a);
	_aligned_free (a);
}

void flush_bps (BPS a)
{
	memset (a->infilt, 0, 2 * a->size * sizeof (complex));
}

void xbps (BPS a, int pos)
{
	int i;
	double I, Q;
	if (a->run && pos == a->position)
	{
		memcpy (&(a->infilt[2 * a->size]), a->in, a->size * sizeof (complex));
		fftw_execute (a->CFor);
		for (i = 0; i < 2 * a->size; i++)
		{
			I = a->gain * a->product[2 * i + 0];
			Q = a->gain * a->product[2 * i + 1];
			a->product[2 * i + 0] = I * a->mults[2 * i + 0] - Q * a->mults[2 * i + 1];
			a->product[2 * i + 1] = I * a->mults[2 * i + 1] + Q * a->mults[2 * i + 0];
		}
		fftw_execute (a->CRev);
		memcpy (a->infilt, &(a->infilt[2 * a->size]), a->size * sizeof(complex));
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_bps (BPS a, double* in, double* out)
{
	decalc_bps (a);
	a->in = in;
	a->out = out;
	calc_bps (a);
}

void setSamplerate_bps (BPS a, int rate)
{
	decalc_bps (a);
	a->samplerate = rate;
	calc_bps (a);
}

void setSize_bps (BPS a, int size)
{
	decalc_bps (a);
	a->size = size;
	calc_bps (a);
}

void setFreqs_bps (BPS a, double f_low, double f_high)
{
	decalc_bps (a);
	a->f_low = f_low;
	a->f_high = f_high;
	calc_bps (a);
}

/********************************************************************************************************
*																										*
*								Overlap-Save Bandpass:  RXA Properties									*
*																										*
********************************************************************************************************/
/*	// UNCOMMENT properties when a pointer is in place in rxa[channel]
PORT
void SetRXABPSRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].bp1.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXABPSFreqs (int channel, double f_low, double f_high)
{
	double* impulse;
	BPS a1;
	EnterCriticalSection (&ch[channel].csDSP);
	a1 = rxa[channel].bp1.p;
	if ((f_low != a1->f_low) || (f_high != a1->f_high))
	{
		a1->f_low = f_low;
		a1->f_high = f_high;
		_aligned_free (a1->mults);
		impulse = fir_bandpass(a1->size + 1, f_low, f_high, a1->samplerate, a1->wintype, 1, 1.0 / (double)(2 * a1->size));
		a1->mults = fftcv_mults (2 * a1->size, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXABPSWindow (int channel, int wintype)
{
	double* impulse;
	BPS a1;
	EnterCriticalSection (&ch[channel].csDSP);
	a1 = rxa[channel].bp1.p;
	if ((a1->wintype != wintype))
	{
		a1->wintype = wintype;
		_aligned_free (a1->mults);
		impulse = fir_bandpass(a1->size + 1, a1->f_low, a1->f_high, a1->samplerate, a1->wintype, 1, 1.0 / (double)(2 * a1->size));
		a1->mults = fftcv_mults (2 * a1->size, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}
*/
/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/
/*	// UNCOMMENT properties when pointers in place in txa[channel]
PORT
void SetTXABPSRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].bp1.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXABPSFreqs (int channel, double f_low, double f_high)
{
	double* impulse;
	BPS a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].bp0.p;
	if ((f_low != a->f_low) || (f_high != a->f_high))
	{
		a->f_low = f_low;
		a->f_high = f_high;
		_aligned_free (a->mults);
		impulse = fir_bandpass(a->size + 1, f_low, f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp1.p;
	if ((f_low != a->f_low) || (f_high != a->f_high))
	{
		a->f_low = f_low;
		a->f_high = f_high;
		_aligned_free (a->mults);
		impulse = fir_bandpass(a->size + 1, f_low, f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp2.p;
	if ((f_low != a->f_low) || (f_high != a->f_high))
	{
		a->f_low = f_low;
		a->f_high = f_high;
		_aligned_free (a->mults);
		impulse = fir_bandpass(a->size + 1, f_low, f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXABPSWindow (int channel, int wintype)
{
	double* impulse;
	BPS a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].bp0.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		_aligned_free (a->mults);
		impulse = fir_bandpass(a->size + 1, a->f_low, a->f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp1.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		_aligned_free (a->mults);
		impulse = fir_bandpass(a->size + 1, a->f_low, a->f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp2.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		_aligned_free (a->mults);
		impulse = fir_bandpass (a->size + 1, a->f_low, a->f_high, a->samplerate, a->wintype, 1, 1.0 / (double)(2 * a->size));
		a->mults = fftcv_mults (2 * a->size, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}
*/

/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Bandpass									*
*																										*
********************************************************************************************************/

BANDPASS create_bandpass (int run, int position, int size, int nc, int mp, double* in, double* out, 
	double f_low, double f_high, int samplerate, int wintype, double gain)
{
	// NOTE:  'nc' must be >= 'size'
	BANDPASS a = (BANDPASS) malloc0 (sizeof (bandpass));
	double* impulse;
	a->run = run;
	a->position = position;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->in = in;
	a->out = out;
	a->f_low = f_low;
	a->f_high = f_high;
	a->samplerate = samplerate;
	a->wintype = wintype;
	a->gain = gain;
	impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
	a->p = create_fircore (a->size, a->in, a->out, a->nc, a->mp, 16, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_bandpass (BANDPASS a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_bandpass (BANDPASS a)
{
	flush_fircore (a->p);
}

void xbandpass (BANDPASS a, int pos)
{
	if (a->run && a->position == pos)
		xfircore (a->p);
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_bandpass (BANDPASS a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_bandpass (BANDPASS a, int rate)
{
	double* impulse;
	a->samplerate = rate;
	impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void setSize_bandpass (BANDPASS a, int size)
{
	// NOTE:  'size' must be <= 'nc'
	double* impulse;
	a->size = size;
	setSize_fircore (a->p, a->size);
	// recalc impulse because scale factor is a function of size
	impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void setGain_bandpass (BANDPASS a, double gain, int update)
{
	double* impulse;
	a->gain = gain;
	impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
	setImpulse_fircore (a->p, impulse, update);
	_aligned_free (impulse);
}

void CalcBandpassFilter (BANDPASS a, double f_low, double f_high, double gain)
{
	double* impulse;
	if ((a->f_low != f_low) || (a->f_high != f_high) || (a->gain != gain))
	{
		a->f_low = f_low;
		a->f_high = f_high;
		a->gain = gain;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXABandpassRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].bp1.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXABandpassFreqs (int channel, double f_low, double f_high)
{
	double* impulse;
	BANDPASS a = rxa[channel].bp1.p;
	if ((f_low != a->f_low) || (f_high != a->f_high))
	{
		impulse = fir_bandpass (a->nc, f_low, f_high, a->samplerate, 
			a->wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 0);
		_aligned_free (impulse);
		EnterCriticalSection (&ch[channel].csDSP);
		a->f_low = f_low;
		a->f_high = f_high;
		setUpdate_fircore (a->p);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

PORT
void SetRXABandpassWindow (int channel, int wintype)
{
	double* impulse;
	BANDPASS a = rxa[channel].bp1.p;
	if ((a->wintype != wintype))
	{
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, 
			wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 0);
		_aligned_free (impulse);
		EnterCriticalSection (&ch[channel].csDSP);
		a->wintype = wintype;
		setUpdate_fircore (a->p);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

PORT
void SetRXABandpassNC (int channel, int nc)
{
	// NOTE:  'nc' must be >= 'size'
	double* impulse;
	BANDPASS a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].bp1.p;
	if (nc != a->nc)
	{
		a->nc = nc;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXABandpassMP (int channel, int mp)
{
	BANDPASS a;
	a = rxa[channel].bp1.p;
	if (mp != a->mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
}

/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetTXABandpassRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].bp1.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

//PORT
//void SetTXABandpassFreqs (int channel, double f_low, double f_high)
//{
//	double* impulse;
//	BANDPASS a;
//	a = txa[channel].bp0.p;
//	if ((f_low != a->f_low) || (f_high != a->f_high))
//	{
//		a->f_low = f_low;
//		a->f_high = f_high;
//		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
//		setImpulse_fircore (a->p, impulse, 1);
//		_aligned_free (impulse);
//	}
//	a = txa[channel].bp1.p;
//	if ((f_low != a->f_low) || (f_high != a->f_high))
//	{
//		a->f_low = f_low;
//		a->f_high = f_high;
//		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
//		setImpulse_fircore (a->p, impulse, 1);
//		_aligned_free (impulse);
//	}
//	a = txa[channel].bp2.p;
//	if ((f_low != a->f_low) || (f_high != a->f_high))
//	{
//		a->f_low = f_low;
//		a->f_high = f_high;
//		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
//		setImpulse_fircore (a->p, impulse, 1);
//		_aligned_free (impulse);
//	}
//}

PORT
void SetTXABandpassWindow (int channel, int wintype)
{
	double* impulse;
	BANDPASS a;
	a = txa[channel].bp0.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
	a = txa[channel].bp1.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
	a = txa[channel].bp2.p;
	if (a->wintype != wintype)
	{
		a->wintype = wintype;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setImpulse_fircore (a->p, impulse, 1);
		_aligned_free (impulse);
	}
}

PORT
void SetTXABandpassNC (int channel, int nc)
{
	// NOTE:  'nc' must be >= 'size'
	double* impulse;
	BANDPASS a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].bp0.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp1.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	a = txa[channel].bp2.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		impulse = fir_bandpass (a->nc, a->f_low, a->f_high, a->samplerate, a->wintype, 1, a->gain / (double)(2 * a->size));
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXABandpassMP (int channel, int mp)
{
	BANDPASS a;
	a = txa[channel].bp0.p;
	if (mp != a->mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
	a = txa[channel].bp1.p;
	if (mp != a->mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
	a = txa[channel].bp2.p;
	if (mp != a->mp)
	{
		a->mp = mp;
		setMp_fircore (a->p, a->mp);
	}
}