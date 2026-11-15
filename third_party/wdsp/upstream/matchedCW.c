/*  matchedCW.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2025-2026 Warren Pratt, NR0V

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

#define _CRT_SECURE_NO_WARNINGS

#include "comm.h"

static int calc_size (double rate, double fwhm)
{
	double nc_d = 1.2067 * rate / fwhm;
	int nc = (int)(round(nc_d * 0.5) * 2.0);
	nc--;
	nc |= nc >> 1;
	nc |= nc >> 2;
	nc |= nc >> 4;
	nc |= nc >> 8;
	nc |= nc >> 16;
	nc++;
	if (nc < 1024) nc = 1024;
	return nc;
}

double* build_matched (int nc, double rate, double f, double fwhm, double scale, int imp_pos)
{
	// nc - number of impulse response values, POWER OF TWO
	//     NOT to be confused with 'ncoef', the number of non-zero impulse response values needed.
	// rate - sample_rate (samples/second)
	// f - center frequency (Hz)
	// fwhm - bandwidth (Hz)
	// scale - scale factor to apply to impulse response
	// position - '0' places the impulse response in the left side of the output field;
	//    '1' centers the impulse response in the output field
	double nc_d = 1.2067 * rate / fwhm;
	int ncoef = (int)(round(nc_d * 0.5) * 2.0);
	double* c_impulse = (double*)malloc0 (nc * sizeof(complex));
	double w_osc = -2.0 * PI * f / rate;
	double posi, posj, m;
	double coef = 1.0;
	double sum = 0.0;
	double norm;
	int i, j, k;
	switch (imp_pos)
	{
	case 0:
	default:
		m = 0.5 * (double)(nc - 1);
		for (i = (ncoef + 1) / 2, j = (ncoef / 2) - 1, k = 0; k < ncoef / 2; i++, j--, k++)
		{
			posi = (double)i - m;
			posj = (double)j - m;
			c_impulse[2 * i + 0] = +coef * cos(posi * w_osc);
			c_impulse[2 * i + 1] = -coef * sin(posi * w_osc);
			c_impulse[2 * j + 0] = +coef * cos(posj * w_osc);
			c_impulse[2 * j + 1] = -coef * sin(posj * w_osc);
			sum += sqrt(c_impulse[2 * i + 0] * c_impulse[2 * i + 0] + c_impulse[2 * i + 1] * c_impulse[2 * i + 1]) +
				sqrt(c_impulse[2 * j + 0] * c_impulse[2 * j + 0] + c_impulse[2 * j + 1] * c_impulse[2 * j + 1]);
		}
		norm = scale / sum;
		for (i = (ncoef + 1) / 2, j = (ncoef / 2) - 1, k = 0; k < ncoef / 2; i++, j--, k++)
		{
			c_impulse[2 * i + 0] *= norm;
			c_impulse[2 * i + 1] *= norm;
			c_impulse[2 * j + 0] *= norm;
			c_impulse[2 * j + 1] *= norm;
		}
		break;
	case 1:
		m = 0.5 * (double)(nc - 1);
		for (i = (nc + 1) / 2, j = (nc / 2) - 1, k = 0; k < ncoef / 2; i++, j--, k++)
		{
			posi = (double)i - m;
			posj = (double)j - m;
			c_impulse[2 * i + 0] = +coef * cos(posi * w_osc);
			c_impulse[2 * i + 1] = -coef * sin(posi * w_osc);
			c_impulse[2 * j + 0] = +coef * cos(posj * w_osc);
			c_impulse[2 * j + 1] = -coef * sin(posj * w_osc);
			sum += sqrt(c_impulse[2 * i + 0] * c_impulse[2 * i + 0] + c_impulse[2 * i + 1] * c_impulse[2 * i + 1]) +
				sqrt(c_impulse[2 * j + 0] * c_impulse[2 * j + 0] + c_impulse[2 * j + 1] * c_impulse[2 * j + 1]);
		}
		norm = scale / sum;
		for (i = (nc + 1) / 2, j = (nc / 2) - 1, k = 0; k < ncoef / 2; i++, j--, k++)
		{
			c_impulse[2 * i + 0] *= norm;
			c_impulse[2 * i + 1] *= norm;
			c_impulse[2 * j + 0] *= norm;
			c_impulse[2 * j + 1] *= norm;
		}
		break;
	}
	// print_impulse("c_matched.txt", nc, c_impulse, 1, 0);
	return c_impulse;
}


/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Matched									*
*																										*
********************************************************************************************************/

MATCHED create_matched (int run, int position, int size, double* in, double* out,
	double f_center, double bandwidth, int samplerate, double gain, int mode)
{
	MATCHED a = (MATCHED) malloc0 (sizeof(matched));
	double* impulse;
	a->run = run;
	a->position = position;
	a->size = size;
	a->in = in;
	a->out = out;
	a->f_center = f_center;
	a->bandwidth = bandwidth;
	a->samplerate = samplerate;
	a->gain = gain;
	a->scale = a->gain / (double)(2 * a->size);
	a->mode = mode;
	a->nc = calc_size (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	impulse = build_matched (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale, 0);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, 0, 4, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_matched (MATCHED a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_matched (MATCHED a)
{
	flush_fircore (a->p);
}

void xmatched (MATCHED a, int pos)
{
	if (a->run && a->position == pos)
	{
		// 'mode == 0' => CWL
		if (a->mode == 1)	// CWU
		{
			for (int i = 0; i < a->size; i++)
				a->in[2 * i + 1] *= -1.0;
		}
		if (a->mode == 2)	// CWL + CWU
		{
			for (int i = 0; i < a->size; i++)
				a->in[2 * i + 1] = a->in[2 * i + 0];
		}
		xfircore (a->p);
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof(complex));
}

void setBuffers_matched (MATCHED a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_matched (MATCHED a, int rate)
{
	a->samplerate = rate;
	int nc = a->nc;
	a->nc = calc_size (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_matched (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale, 0);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free(impulse);
}

void setSize_matched (MATCHED a, int size)
{
	a->size = size;
	setSize_fircore (a->p, a->size);
	a->scale = a->gain / (double)(2 * a->size);
	int nc = a->nc;
	a->nc = calc_size (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_matched (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale, 0);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free (impulse);
}

void setGain_matched (MATCHED a, double gain)
{
	a->gain = gain;
	a->scale = a->gain / (double)(2 * a->size);
	double* impulse = build_matched (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale, 0);
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void CalcMatchedFilter (MATCHED a, double f_center, double bandwidth, double gain)
{
	if ((a->f_center != f_center) || (a->bandwidth != bandwidth) || (a->gain != gain))
	{
		a->f_center = f_center;
		a->bandwidth = bandwidth;
		a->gain = gain;
		a->scale = a->gain / (double)(2 * a->size);
		int nc = a->nc;
		a->nc = calc_size (a->samplerate, a->bandwidth);
		if (a->size > a->nc) a->nc = a->size;
		double* impulse = build_matched (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale, 0);
		if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
		else             setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXAMatchedRun (int channel, int run)
{
	MATCHED a = rxa[channel].matched.p;
	EnterCriticalSection (&ch[channel].csDSP);
	a->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAMatchedFreqs (int channel, double f_center, double bandwidth)
{
	MATCHED a = rxa[channel].matched.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcMatchedFilter (a, f_center, bandwidth, a->gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAMatchedGain (int channel, double gain)
{
	MATCHED a = rxa[channel].matched.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcMatchedFilter (a, a->f_center, a->bandwidth, gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}
