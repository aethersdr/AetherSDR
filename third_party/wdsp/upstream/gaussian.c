/*  gaussian.c

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

static int calc_nc (double fwhm, double nsigma, double rate)
{
	int nc;
	double fsigma = fwhm / 2.35482;
	double sigma = 1.0 / (2.0 * PI * fsigma);
	nc = (int)ceil (2.0 * nsigma * sigma * rate);
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

double* build_gaussian(int nc, double rate, double f, double fwhm, double scale, double nsigma)
{
	// nc - number of impulse response values, IS EVEN FOR THE FOLLOWING CODE
	// rate - sample_rate (samples/second)
	// f - center frequency (Hz)
	// fwhm - bandwidth (Hz)
	// scale - scale factor to apply to impulse response
	// nsigma - number of sigma to extend on each side of center
	double fsigma = fwhm / 2.35482;
	double sigma = 1.0 / (2.0 * PI * fsigma);
	double* impulse = (double*)malloc0 (nc * sizeof(double));
	double delta = 1.0 / rate;
	int i, j;
	double x, y;
	double gmult = 1.0 / (sqrt (2.0 * PI) * sigma);
	double gdiv = 1.0 / (2.0 * sigma * sigma);
	double sum = 0.0;
	for (i = 0, y = -((double)(nc - 1) / 2.0); i < nc; i++, y += 1.0)
	{
		x = y * delta;
		impulse[i] = gmult * exp (-(x * x) * gdiv);
		sum += impulse[i];
	}
	for (i = 0; i < nc; i++)
	{
		impulse[i] *= (scale / sum);
	}
	// print_impulse("gaussian.txt", nc, impulse, 0, 0);
	double* c_impulse = (double*)malloc0 (nc * sizeof(complex));
	double w_osc = -2.0 * PI * f / rate;
	double m = 0.5 * (double)(nc - 1);
	double posi, posj;
	double coef;
	for (i = (nc + 1) / 2, j = nc / 2 - 1; i < nc; i++, j--)
	{
		posi = (double)i - m;
		posj = (double)j - m;
		coef = impulse[j];
		c_impulse[2 * i + 0] = +coef * cos(posi * w_osc);
		c_impulse[2 * i + 1] = -coef * sin(posi * w_osc);
		c_impulse[2 * j + 0] = +coef * cos(posj * w_osc);
		c_impulse[2 * j + 1] = -coef * sin(posj * w_osc);
	}
	// print_impulse("c_gaussian.txt", nc, c_impulse, 1, 0);
	_aligned_free (impulse);
	return c_impulse;
}


/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Gaussian									*
*																										*
********************************************************************************************************/

GAUSSIAN create_gaussian (int run, int position, int size, int nc, double* in, double* out,
	double f_center, double bandwidth, int samplerate, double gain, double nsigma, int mode)
{
	GAUSSIAN a = (GAUSSIAN)malloc0 (sizeof(gaussian));
	double* impulse;
	a->run = run;
	a->position = position;
	a->size = size;
	a->nc = nc;
	a->in = in;
	a->out = out;
	a->f_center = f_center;
	a->bandwidth = bandwidth;
	a->samplerate = samplerate;
	a->gain = gain;
	a->scale = a->gain / (double)(2 * a->size);
	a->nsigma = nsigma;
	a->mode = mode;
	a->nc_default = a->nc;
	if (a->nc == 0) a->nc = calc_nc (a->bandwidth, a->nsigma, a->samplerate);
	if (a->size > a->nc) a->nc = a->size;
	impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, 0, 4, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_gaussian (GAUSSIAN a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_gaussian (GAUSSIAN a)
{
	flush_fircore (a->p);
}

void xgaussian (GAUSSIAN a, int pos)
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
		xfircore(a->p);
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof(complex));
}

void setBuffers_gaussian (GAUSSIAN a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_gaussian (GAUSSIAN a, int rate)
{
	a->samplerate = rate;
	int nc = a->nc;
	a->nc = a->nc_default;
	if (a->nc == 0) a->nc = calc_nc (a->bandwidth, a->nsigma, a->samplerate);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free (impulse);
}

void setSize_gaussian (GAUSSIAN a, int size)
{
	a->size = size;
	setSize_fircore (a->p, a->size);
	a->scale = a->gain / (double)(2 * a->size);
	int nc = a->nc;
	a->nc = a->nc_default;
	if (a->nc == 0) a->nc = calc_nc (a->bandwidth, a->nsigma, a->samplerate);
	if (a->size > a->nc)  a->nc = a->size;
	double* impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free (impulse);
}

void setGain_gaussian (GAUSSIAN a, double gain)
{
	a->gain = gain;
	a->scale = a->gain / (double)(2 * a->size);
	double* impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void CalcGaussianFilter (GAUSSIAN a, double f_center, double bandwidth, double gain)
{
	double* impulse;
	if ((a->f_center != f_center) || (a->bandwidth != bandwidth) || (a->gain != gain))
	{
		a->f_center = f_center;
		a->bandwidth = bandwidth;
		a->gain = gain;
		a->scale = a->gain / (double)(2 * a->size);
		int nc = a->nc;
		a->nc = a->nc_default;
		if (a->nc == 0) a->nc = calc_nc (a->bandwidth, a->nsigma, a->samplerate);
		if (a->size > a->nc) a->nc = a->size;
		impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
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
void SetRXAGaussianRun (int channel, int run)
{
	GAUSSIAN a = rxa[channel].gaussian.p;
	EnterCriticalSection (&ch[channel].csDSP);
	a->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGaussianFreqs (int channel, double f_center, double bandwidth)
{
	GAUSSIAN a = rxa[channel].gaussian.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcGaussianFilter (a, f_center, bandwidth, a->gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGaussianGain (int channel, double gain)
{
	GAUSSIAN a = rxa[channel].gaussian.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcGaussianFilter (a, a->f_center, a->bandwidth, gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGaussianNC (int channel, int nc)
{
	double* impulse;
	GAUSSIAN a = rxa[channel].gaussian.p;
	EnterCriticalSection (&ch[channel].csDSP);
	if (nc != a->nc || nc == 0)
	{
		a->nc = nc;
		a->nc_default = a->nc;
		if (a->nc == 0) a->nc = calc_nc (a->bandwidth, a->nsigma, a->samplerate);
		if (a->size > a->nc) a->nc = a->size;
		impulse = build_gaussian (a->nc, (double)a->samplerate, a->f_center, a->bandwidth, a->scale, a->nsigma);
		setNc_fircore (a->p, a->nc, impulse);
		_aligned_free (impulse);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}
