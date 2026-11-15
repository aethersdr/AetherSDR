/*  doublepole.c

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

static int calc_dpole_nc (double rate, double bandwidth)
{
	int nc = 0;
	int rate_mult = (int)ceil((int)rate / 12000);
	int bw_mult = 1;
	bandwidth /= 1.7;
	if (bandwidth < 80.0) bw_mult = 2;
	if (bandwidth < 40.0) bw_mult = 4;
	if (bandwidth < 20.0) bw_mult = 8;
	if (bandwidth < 10.0) bw_mult = 16;
	nc = 256 * rate_mult * bw_mult;;
	if (nc < 2048) nc = 2048;
	return nc;
}

static void H (double scale, double fcenter, double bandwidth, double f, double Hres[])
{
	double a[2];
	double b[2];
	a[0] = (bandwidth / fcenter);
	a[1] = 0.0;
	b[0] = 1.0 - (f / fcenter) * (f / fcenter);
	b[1] = f * bandwidth / (fcenter * fcenter);
	cdiv (a, b, Hres);
	return;
}

double* build_doublepole_1sided (int nc, double rate, double fcenter, double bandwidth, double scale)
{
	// nc - number of impulse response values, POWER OF TWO
	// rate - sample_rate (samples/second)
	// f - center frequency (Hz)
	// bandwidth - bandwidth (Hz)
	// scale - scale factor to apply to impulse response
	double Hres[2] = { 0.0 };
	int i;
	double jd;
	double nfreqs = 3000.0;
	double* h_i = (double*)malloc0 (nc * sizeof(complex));
	for (i = 0; i < nc; i++)
	{
		double sum[2] = { 0.0 };
		double eto[2] = { 0.0 };
		double inner[2] = { 0.0 };
		for (jd = -nfreqs / 2.0; jd <= nfreqs / 2.0; jd += 1.0)
		{
			double theta = 2.0 * PI * (double)i * jd / rate;
			eto[0] = cos(theta);
			eto[1] = sin(theta);
			H(scale, fcenter, bandwidth, jd, Hres);
			cmult(Hres, eto, inner);
			sum[0] += inner[0];
			sum[1] += inner[1];
		}
		h_i[2 * i + 0] = sum[0] / (double)nc;
		h_i[2 * i + 1] = 0.0;
	}
	// print_impulse("pre_analytic.txt", nc, h_i, 1, 0);
	int npad = 8;
	int size = npad * nc;
	double* pad = (double*)malloc0 (size * sizeof(complex));
	memcpy (pad, h_i, nc * sizeof(complex));
	analytic (size, pad, pad);
	memcpy (h_i, pad, nc * sizeof(complex));
	_aligned_free (pad);
	double sum = 0.0;
	for (i = 0; i < nc; i++)
		sum += sqrt(h_i[2 * i + 0] * h_i[2 * i + 0] + h_i[2 * i + 1] * h_i[2 * i + 1]);
	for (i = 0; i < 2 * nc; i++)
		h_i[i] *= scale / sum;
	// print_impulse("dpole.txt", nc, h_i, 1, 0);
	return h_i;
}

double* build_doublepole_2sided (int nc, double rate, double fcenter, double bandwidth, double scale)
{
	// nc - number of impulse response values, POWER OF TWO
	// rate - sample_rate (samples/second)
	// f - center frequency (Hz)
	// bandwidth - bandwidth (Hz)
	// scale - scale factor to apply to impulse response
	double bw = bandwidth / 1.70;
	double Hres[2] = { 0.0 };
	int i;
	double jd;
	double delta = rate / (double)nc;
	int nfreqs = 3000;
	double mult = 2.0 * scale / (double)nc;
	double* h_i = (double*)malloc0 (nc * sizeof(complex));
	double* H_i = (double*)malloc0 (nc * sizeof(complex));
	for (i = 0, jd = 0.0; i < nfreqs / 2; i++, jd += delta)
	{
		H (scale, fcenter, bw, jd, Hres);
		H_i[2 * i + 0] = Hres[0] * mult;
		H_i[2 * i + 1] = Hres[1] * mult;
	}
	for (i = nc - nfreqs / 2, jd = -(double)nfreqs * delta; i < nc; i++, jd += delta)
	{
		H (scale, fcenter, bw, jd, Hres);
		H_i[2 * i + 0] = Hres[0] * mult;
		H_i[2 * i + 1] = Hres[1] * mult;
	}
	fftw_plan prev = fftw_plan_dft_1d (nc, (fftw_complex*)H_i,
		(fftw_complex*)h_i, FFTW_BACKWARD, FFTW_PATIENT);
	fftw_execute      (prev);
	fftw_destroy_plan (prev);
	_aligned_free     (H_i);
	for (i = 0; i < nc; i++)
		h_i[2 * i + 1] = 0.0;
	return h_i;
}

double* build_doublepole_1eff (int nc, double rate, double fcenter, double bandwidth, double scale)
{
	// nc - number of impulse response values, POWER OF TWO
	// rate - sample_rate (samples/second)
	// f - center frequency (Hz)
	// bandwidth - bandwidth (Hz)
	// scale - scale factor to apply to impulse response
	double bw = bandwidth / 1.7;
	double alpha = PI * bw / rate;
	double omega = - TWOPI * fcenter / rate;
	double impulse, arg;
	double* c_impulse = (double*) malloc0 (nc * sizeof(complex));
	for (int i = 0; i < nc; i++)
	{
		impulse = scale * alpha * exp (-alpha * (double)i);
		arg = omega * (double)i;
		c_impulse[2 * i + 0] = + impulse * cos (arg);
		c_impulse[2 * i + 1] = - impulse * sin (arg);
	}
	return c_impulse;
}

DOUBLEPOLE create_doublepole (int run, int position, int size, double* in, double* out,
	double f_center, double bandwidth, int samplerate, double gain, int mode)
{
	DOUBLEPOLE a = (DOUBLEPOLE)malloc0 (sizeof(doublepole));
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
	a->nc = calc_dpole_nc (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_doublepole_1eff (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, 0, 4, impulse);
	_aligned_free (impulse);
	return a;
}

void destroy_doublepole (DOUBLEPOLE a)
{
	destroy_fircore (a->p);
	_aligned_free (a);
}

void flush_doublepole (DOUBLEPOLE a)
{
	flush_fircore (a->p);
}

void xdoublepole (DOUBLEPOLE a, int pos)
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

void setBuffers_doublepole (DOUBLEPOLE a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_doublepole (DOUBLEPOLE a, int rate)
{
	a->samplerate = rate;
	int nc = a->nc;
	a->nc = calc_dpole_nc (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_doublepole_1eff (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free (impulse);
}

void setSize_doublepole (DOUBLEPOLE a, int size)
{
	a->size = size;
	setSize_fircore (a->p, a->size);
	a->scale = a->gain / (double)(2 * a->size);
	int nc = a->nc;
	a->nc = calc_dpole_nc (a->samplerate, a->bandwidth);
	if (a->size > a->nc) a->nc = a->size;
	double* impulse = build_doublepole_1eff (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale);
	if (nc == a->nc) setImpulse_fircore (a->p, impulse, 1);
	else             setNc_fircore (a->p, a->nc, impulse);
	_aligned_free (impulse);
}

void setGain_doublepole (DOUBLEPOLE a, double gain)
{
	a->gain = gain;
	a->scale = a->gain / (double)(2 * a->size);
	double* impulse = build_doublepole_1eff (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale);
	setImpulse_fircore (a->p, impulse, 1);
	_aligned_free (impulse);
}

void CalcDoublepoleFilter (DOUBLEPOLE a, double f_center, double bandwidth, double gain)
{
	if ((a->f_center != f_center) || (a->bandwidth != bandwidth) || (a->gain != gain))
	{
		a->f_center = f_center;
		a->bandwidth = bandwidth;
		a->gain = gain;
		a->scale = a->gain / (double)(2 * a->size);
		int nc = a->nc;
		a->nc = calc_dpole_nc (a->samplerate, a->bandwidth);
		if (a->size > a->nc) a->nc = a->size;
		double* impulse = build_doublepole_1eff (a->nc, a->samplerate, a->f_center, a->bandwidth, a->scale);
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
void SetRXADoublepoleRun (int channel, int run)
{
	DOUBLEPOLE a = rxa[channel].doublepole.p;
	EnterCriticalSection (&ch[channel].csDSP);
	a->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXADoublepoleFreqs (int channel, double f_center, double bandwidth)
{
	DOUBLEPOLE a = rxa[channel].doublepole.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcDoublepoleFilter (a, f_center, bandwidth, a->gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXADoublepoleGain (int channel, double gain)
{
	DOUBLEPOLE a = rxa[channel].doublepole.p;
	EnterCriticalSection (&ch[channel].csDSP);
	CalcDoublepoleFilter (a, a->f_center, a->bandwidth, gain);
	LeaveCriticalSection (&ch[channel].csDSP);
}
