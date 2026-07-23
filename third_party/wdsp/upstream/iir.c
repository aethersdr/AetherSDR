/*  iir.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014, 2022, 2023, 2026 Warren Pratt, NR0V

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
*											Bi-Quad Notch												*
*																										*
********************************************************************************************************/

void calc_snotch (SNOTCH a)
{
	double fn, qk, qr, csn;
	fn = a->f / (double)a->rate;
	csn = cos (TWOPI * fn);
	qr = 1.0 - 3.0 * a->bw;
	qk = (1.0 - 2.0 * qr * csn + qr * qr) / (2.0 * (1.0 - csn));
	a->a0 = + qk;
	a->a1 = - 2.0 * qk * csn;
	a->a2 = + qk;
	a->b1 = + 2.0 * qr * csn;
	a->b2 = - qr * qr;
	flush_snotch (a);
}

SNOTCH create_snotch (int run, int size, double* in, double* out, int rate, double f, double bw)
{
	SNOTCH a = (SNOTCH) malloc0 (sizeof (snotch));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->f = f;
	a->bw = bw;
	InitializeCriticalSectionAndSpinCount ( &a->cs_update, 2500 );
	calc_snotch (a);
	return a;
}

void destroy_snotch (SNOTCH a)
{
	DeleteCriticalSection (&a->cs_update);
	_aligned_free (a);
}

void flush_snotch (SNOTCH a)
{
	a->x1 = a->x2 = a->y1 = a->y2 = 0.0;
}

void xsnotch (SNOTCH a)
{
	EnterCriticalSection (&a->cs_update);
	if (a->run)
	{
		int i;
		for (i = 0; i < a->size; i++)
		{
			a->x0 = a->in[2 * i + 0];
			a->out[2 * i + 0] = a->a0 * a->x0 + a->a1 * a->x1 + a->a2 * a->x2 + a->b1 * a->y1 + a->b2 * a->y2;
			a->y2 = a->y1;
			a->y1 = a->out[2 * i + 0];
			a->x2 = a->x1;
			a->x1 = a->x0;
		}
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof (complex));
	LeaveCriticalSection (&a->cs_update);
}

void setBuffers_snotch (SNOTCH a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_snotch (SNOTCH a, int rate)
{
	a->rate = rate;
	calc_snotch (a);
}

void setSize_snotch (SNOTCH a, int size)
{
	a->size = size;
	flush_snotch (a);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

void SetSNCTCSSFreq (SNOTCH a, double freq)
{
	EnterCriticalSection (&a->cs_update);
	a->f = freq;
	calc_snotch (a);
	LeaveCriticalSection (&a->cs_update);
}

void SetSNCTCSSRun (SNOTCH a, int run)
{
	EnterCriticalSection (&a->cs_update);
	a->run = run;
	LeaveCriticalSection (&a->cs_update);
}


/********************************************************************************************************
*																										*
*											Complex Bi-Quad Peaking										*
*																										*
********************************************************************************************************/

void calc_speak (SPEAK a)
{
	double ratio;	
	double f_corr, g_corr, bw_corr, bw_parm, A, f_min;
	
	switch (a->design)
	{
	case 0:
		ratio = a->bw / a->f;
		switch (a->nstages)
		{
		case 4:
			bw_parm = 2.4;
			f_corr  = 1.0 - 0.160 * ratio + 1.440 * ratio * ratio;
			g_corr = 1.0 - 1.003 * ratio + 3.990 * ratio * ratio;
			break;
		default:
			bw_parm = 1.0;
			f_corr  = 1.0;
			g_corr = 1.0;
			break;
		}
		{
			double fn, qk, qr, csn;
			a->fgain = a->gain / g_corr;
			fn = a->f / (double)a->rate / f_corr;
			csn = cos (TWOPI * fn);
			qr = 1.0 - 3.0 * a->bw / (double)a->rate * bw_parm;
			qk = (1.0 - 2.0 * qr * csn + qr * qr) / (2.0 * (1.0 - csn));
			a->a0 = 1.0 - qk;
			a->a1 = 2.0 * (qk - qr) * csn;
			a->a2 = qr * qr - qk;
			a->b1 = 2.0 * qr * csn;
			a->b2 = - qr * qr;
		}
		break;

	case 1:
		if (a->f < 200.0) a->f = 200.0;
		ratio = a->bw / a->f;
		switch (a->nstages)
		{
		case 4:
			bw_parm = 5.0;
			bw_corr = 1.13 * ratio - 0.956 * ratio * ratio;
			A = 2.5;
			f_min = 50.0;
			break;
		default:
			bw_parm = 1.0;
			bw_corr = 1.0;
			A = 2.5;
			f_min = 50.0;
			break;
		}
		{
			double w0, sn, c, den;
			if (a->f < f_min) a->f = f_min;
			w0 = TWOPI * a->f / (double)a->rate;
			sn = sin (w0);
			a->cbw = bw_corr * a->f;
			c = sn * sinh(0.5 * log((a->f + 0.5 * a->cbw * bw_parm) / (a->f - 0.5 * a->cbw * bw_parm)) * w0 / sn);
			den = 1.0 + c / A;
			a->a0 = (1.0 + c * A) / den;
			a->a1 = - 2.0 * cos (w0) / den;
			a->a2 = (1 - c * A) / den;
			a->b1 = - a->a1;
			a->b2 = - (1 - c / A ) / den;
			a->fgain = a->gain / pow (A * A, (double)a->nstages);
		}
		break;
	}
	flush_speak (a);
}

SPEAK create_speak (int run, int size, double* in, double* out, int rate, double f, double bw, double gain, int nstages, int design)
{
	SPEAK a = (SPEAK) malloc0 (sizeof (speak));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->f = f;
	a->bw = bw;
	a->gain = gain;
	a->nstages = nstages;
	a->design = design;
	a->x0 = (double *) malloc0 (a->nstages * sizeof (complex));
	a->x1 = (double *) malloc0 (a->nstages * sizeof (complex));
	a->x2 = (double *) malloc0 (a->nstages * sizeof (complex));
	a->y0 = (double *) malloc0 (a->nstages * sizeof (complex));
	a->y1 = (double *) malloc0 (a->nstages * sizeof (complex));
	a->y2 = (double *) malloc0 (a->nstages * sizeof (complex));
	InitializeCriticalSectionAndSpinCount ( &a->cs_update, 2500 );
	calc_speak (a);
	return a;
}

void destroy_speak (SPEAK a)
{
	DeleteCriticalSection (&a->cs_update);
	_aligned_free (a->y2);
	_aligned_free (a->y1);
	_aligned_free (a->y0);
	_aligned_free (a->x2);
	_aligned_free (a->x1);
	_aligned_free (a->x0);
	_aligned_free (a);
}

void flush_speak (SPEAK a)
{
	int i;
	for (i = 0; i < a->nstages; i++)
	{
		a->x1[2 * i + 0] = a->x2[2 * i + 0] = a->y1[2 * i + 0] = a->y2[2 * i + 0] = 0.0;
		a->x1[2 * i + 1] = a->x2[2 * i + 1] = a->y1[2 * i + 1] = a->y2[2 * i + 1] = 0.0;
	}
}

void xspeak (SPEAK a)
{
	EnterCriticalSection (&a->cs_update);
	if (a->run)
	{
		int i, j, n;
		for (i = 0; i < a->size; i++)
		{
			for (j = 0; j < 2; j++)
			{
				a->x0[j] = a->fgain * a->in[2 * i + j];
				for (n = 0; n < a->nstages; n++)
				{
					if (n > 0) a->x0[2 * n + j] = a->y0[2 * (n - 1) + j];
					a->y0[2 * n + j]	= a->a0 * a->x0[2 * n + j] 
										+ a->a1 * a->x1[2 * n + j] 
										+ a->a2 * a->x2[2 * n + j] 
										+ a->b1 * a->y1[2 * n + j] 
										+ a->b2 * a->y2[2 * n + j];
					a->y2[2 * n + j] = a->y1[2 * n + j];
					a->y1[2 * n + j] = a->y0[2 * n + j];
					a->x2[2 * n + j] = a->x1[2 * n + j];
					a->x1[2 * n + j] = a->x0[2 * n + j];
				}
				a->out[2 * i + j] = a->y0[2 * (a->nstages - 1) + j];
			}
		}
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->size * sizeof (complex));
	LeaveCriticalSection (&a->cs_update);
}

void setBuffers_speak (SPEAK a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_speak (SPEAK a, int rate)
{
	a->rate = rate;
	calc_speak (a);
}

void setSize_speak (SPEAK a, int size)
{
	a->size = size;
	flush_speak (a);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXABiQuadRun (int channel, int run)
{
	SPEAK a = rxa[channel].speak.p;
	EnterCriticalSection (&a->cs_update);
	a->run = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXABiQuadFreq (int channel, double freq)
{
	SPEAK a = rxa[channel].speak.p;
	EnterCriticalSection (&a->cs_update);
	a->f = freq;
	calc_speak (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXABiQuadBandwidth (int channel, double bw)
{
	SPEAK a = rxa[channel].speak.p;
	EnterCriticalSection (&a->cs_update);
	a->bw = bw;
	calc_speak (a);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXABiQuadGain (int channel, double gain)
{
	SPEAK a = rxa[channel].speak.p;
	EnterCriticalSection (&a->cs_update);
	a->gain = gain;
	calc_speak (a);
	LeaveCriticalSection (&a->cs_update);
}

/********************************************************************************************************
*																										*
*										Complex Multiple Peaking										*
*																										*
********************************************************************************************************/

void calc_mpeak (MPEAK a)
{
	int i;
	a->tmp = (double *) malloc0 (a->size * sizeof (complex));
	a->mix = (double *) malloc0 (a->size * sizeof (complex));
	for (i = 0; i < a->npeaks; i++)
	{
		a->pfil[i] = create_speak (	1, 
									a->size, 
									a->in, 
									a->tmp, 
									a->rate, 
									a->f[i], 
									a->bw[i], 
									a->gain[i], 
									a->nstages, 
									1 );
	}
}

void decalc_mpeak (MPEAK a)
{
	int i;
	for (i = 0; i < a->npeaks; i++)
		destroy_speak (a->pfil[i]);
	_aligned_free (a->mix);
	_aligned_free (a->tmp);
}

MPEAK create_mpeak (int run, int size, double* in, double* out, int rate, int npeaks, int* enable, double* f, double* bw, double* gain, int nstages)
{
	MPEAK a = (MPEAK) malloc0 (sizeof (mpeak));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->npeaks = npeaks;
	a->nstages = nstages;
	a->enable  = (int *) malloc0 (a->npeaks * sizeof (int));
	a->f    = (double *) malloc0 (a->npeaks * sizeof (double));
	a->bw   = (double *) malloc0 (a->npeaks * sizeof (double));
	a->gain = (double *) malloc0 (a->npeaks * sizeof (double));
	memcpy (a->enable, enable, a->npeaks * sizeof (int));
	memcpy (a->f, f, a->npeaks * sizeof (double));
	memcpy (a->bw, bw, a->npeaks * sizeof (double));
	memcpy (a->gain, gain, a->npeaks * sizeof (double));
	a->pfil = (SPEAK *) malloc0 (a->npeaks * sizeof (SPEAK));
	InitializeCriticalSectionAndSpinCount ( &a->cs_update, 2500 );
	calc_mpeak (a);
	return a;
}

void destroy_mpeak (MPEAK a)
{
	decalc_mpeak (a);
	DeleteCriticalSection (&a->cs_update);
	_aligned_free (a->pfil);
	_aligned_free (a->gain);
	_aligned_free (a->bw);
	_aligned_free (a->f);
	_aligned_free (a->enable);
	_aligned_free (a);
}

void flush_mpeak (MPEAK a)
{
	int i;
	for (i = 0; i < a->npeaks; i++)
		flush_speak (a->pfil[i]);
}

void xmpeak (MPEAK a)
{
	EnterCriticalSection (&a->cs_update);
	if (a->run)
	{
		int i, j;
		memset (a->mix, 0, a->size * sizeof (complex));
		for (i = 0; i < a->npeaks; i++)
		{
			if (a->enable[i])
			{
				xspeak (a->pfil[i]);
				for (j = 0; j < 2 * a->size; j++)
					a->mix[j] += a->tmp[j];
			}
		}
		memcpy (a->out, a->mix, a->size * sizeof (complex));
	}
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
	LeaveCriticalSection (&a->cs_update);
}

void setBuffers_mpeak (MPEAK a, double* in, double* out)
{
	decalc_mpeak (a);
	a->in = in;
	a->out = out;
	calc_mpeak (a);
}

void setSamplerate_mpeak (MPEAK a, int rate)
{
	decalc_mpeak (a);
	a->rate = rate;
	calc_mpeak (a);
}

void setSize_mpeak (MPEAK a, int size)
{
	decalc_mpeak (a);
	a->size = size;
	calc_mpeak (a);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXAmpeakRun (int channel, int run)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->run = run;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXAmpeakNpeaks (int channel, int npeaks)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->npeaks = npeaks;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXAmpeakFilEnable (int channel, int fil, int enable)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->enable[fil] = enable;
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXAmpeakFilFreq (int channel, int fil, double freq)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->f[fil] = freq;
	a->pfil[fil]->f = freq;
	calc_speak(a->pfil[fil]);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXAmpeakFilBw (int channel, int fil, double bw)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->bw[fil] = bw;
	a->pfil[fil]->bw = bw;
	calc_speak(a->pfil[fil]);
	LeaveCriticalSection (&a->cs_update);
}

PORT
void SetRXAmpeakFilGain (int channel, int fil, double gain)
{
	MPEAK a = rxa[channel].mpeak.p;
	EnterCriticalSection (&a->cs_update);
	a->gain[fil] = gain;
	a->pfil[fil]->gain = gain;
	calc_speak(a->pfil[fil]);
	LeaveCriticalSection (&a->cs_update);
}


/********************************************************************************************************
*																										*
*									Complex Bi-Quad Low-Pass				     						*
*																										*
********************************************************************************************************/

void calc_bqlp(BQLP a)
{
	double w0, cs, c, den;
	w0 = TWOPI * a->fc / (double)a->rate;
	cs = cos(w0);
	c = sin(w0) / (2.0 * a->Q);
	den = 1.0 + c;
	a->a0 = 0.5 * (1.0 - cs) / den;
	a->a1 = (1.0 - cs) / den;
	a->a2 = 0.5 * (1.0 - cs) / den;
	a->b1 = 2.0 * cs / den;
	a->b2 = (c - 1.0) / den;
	flush_bqlp(a);
}

BQLP create_bqlp(int run, int size, double* in, double* out, double rate, double fc, double Q, double gain, int nstages)
{
	BQLP a = (BQLP)malloc0(sizeof(bqlp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->fc = fc;
	a->Q = Q;
	a->gain = gain;
	a->nstages = nstages;
	a->x0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->x1 = (double*)malloc0(a->nstages * sizeof(complex));
	a->x2 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y1 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y2 = (double*)malloc0(a->nstages * sizeof(complex));
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_bqlp(a);
	return a;
}

void destroy_bqlp(BQLP a)
{
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a->y2);
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x2);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
	_aligned_free(a);
}

void flush_bqlp(BQLP a)
{
	int i;
	for (i = 0; i < a->nstages; i++)
	{
		a->x1[2 * i + 0] = a->x2[2 * i + 0] = a->y1[2 * i + 0] = a->y2[2 * i + 0] = 0.0;
		a->x1[2 * i + 1] = a->x2[2 * i + 1] = a->y1[2 * i + 1] = a->y2[2 * i + 1] = 0.0;
	}
}

void xbqlp(BQLP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, j, n;
		for (i = 0; i < a->size; i++)
		{
			for (j = 0; j < 2; j++)
			{
				a->x0[j] = a->gain * a->in[2 * i + j];
				for (n = 0; n < a->nstages; n++)
				{
					if (n > 0) a->x0[2 * n + j] = a->y0[2 * (n - 1) + j];
					a->y0[2 * n + j] = a->a0 * a->x0[2 * n + j]
						+ a->a1 * a->x1[2 * n + j]
						+ a->a2 * a->x2[2 * n + j]
						+ a->b1 * a->y1[2 * n + j]
						+ a->b2 * a->y2[2 * n + j];
					a->y2[2 * n + j] = a->y1[2 * n + j];
					a->y1[2 * n + j] = a->y0[2 * n + j];
					a->x2[2 * n + j] = a->x1[2 * n + j];
					a->x1[2 * n + j] = a->x0[2 * n + j];
				}
				a->out[2 * i + j] = a->y0[2 * (a->nstages - 1) + j];
			}
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(complex));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_bqlp(BQLP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_bqlp(BQLP a, int rate)
{
	a->rate = rate;
	calc_bqlp(a);
}

void setSize_bqlp(BQLP a, int size)
{
	a->size = size;
	flush_bqlp(a);
}

/********************************************************************************************************
*																										*
*									   Double Bi-Quad Low-Pass				     						*
*																										*
********************************************************************************************************/

void calc_dbqlp(BQLP a)
{
	double w0, cs, c, den;
	w0 = TWOPI * a->fc / (double)a->rate;
	cs = cos(w0);
	c = sin(w0) / (2.0 * a->Q);
	den = 1.0 + c;
	a->a0 = 0.5 * (1.0 - cs) / den;
	a->a1 = (1.0 - cs) / den;
	a->a2 = 0.5 * (1.0 - cs) / den;
	a->b1 = 2.0 * cs / den;
	a->b2 = (c - 1.0) / den;
	flush_dbqlp(a);
}

BQLP create_dbqlp(int run, int size, double* in, double* out, double rate, double fc, double Q, double gain, int nstages)
{
	BQLP a = (BQLP)malloc0(sizeof(bqlp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->fc = fc;
	a->Q = Q;
	a->gain = gain;
	a->nstages = nstages;
	a->x0 = (double*)malloc0(a->nstages * sizeof(double));
	a->x1 = (double*)malloc0(a->nstages * sizeof(double));
	a->x2 = (double*)malloc0(a->nstages * sizeof(double));
	a->y0 = (double*)malloc0(a->nstages * sizeof(double));
	a->y1 = (double*)malloc0(a->nstages * sizeof(double));
	a->y2 = (double*)malloc0(a->nstages * sizeof(double));
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_dbqlp(a);
	return a;
}

void destroy_dbqlp(BQLP a)
{
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a->y2);
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x2);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
	_aligned_free(a);
}

void flush_dbqlp(BQLP a)
{
	int i;
	for (i = 0; i < a->nstages; i++)
	{
		a->x1[i] = a->x2[i] = a->y1[i] = a->y2[i] = 0.0;
	}
}

void xdbqlp(BQLP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, n;
		for (i = 0; i < a->size; i++)
		{
			a->x0[0] = a->gain * a->in[i];
			for (n = 0; n < a->nstages; n++)
			{
				if (n > 0) a->x0[n] = a->y0[n - 1];
				a->y0[n] = a->a0 * a->x0[n]
					+ a->a1 * a->x1[n]
					+ a->a2 * a->x2[n]
					+ a->b1 * a->y1[n]
					+ a->b2 * a->y2[n];
				a->y2[n] = a->y1[n];
				a->y1[n] = a->y0[n];
				a->x2[n] = a->x1[n];
				a->x1[n] = a->x0[n];
			}
			a->out[i] = a->y0[a->nstages - 1];
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(double));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_dbqlp(BQLP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_dbqlp(BQLP a, int rate)
{
	a->rate = rate;
	calc_dbqlp(a);
}

void setSize_dbqlp(BQLP a, int size)
{
	a->size = size;
	flush_dbqlp(a);
}


/********************************************************************************************************
*																										*
*									Complex Bi-Quad Band-Pass				     						*
*																										*
********************************************************************************************************/

void calc_bqbp(BQBP a)
{
	double f0, w0, bw, q, sn, cs, c, den;
	bw = a->f_high - a->f_low;
	f0 = (a->f_high + a->f_low) / 2.0;
	q = f0 / bw;
	w0 = TWOPI * f0 / a->rate;
	sn = sin(w0);
	cs = cos(w0);
	c = sn / (2.0 * q);
	den = 1.0 + c;
	a->a0 = +c / den;
	a->a1 = 0.0;
	a->a2 = -c / den;
	a->b1 = 2.0 * cs / den;
	a->b2 = (c - 1.0) / den;
	flush_bqbp(a);
}

BQBP create_bqbp(int run, int size, double* in, double* out, double rate, double f_low, double f_high, double gain, int nstages)
{
	BQBP a = (BQBP)malloc0(sizeof(bqbp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->f_low = f_low;
	a->f_high = f_high;
	a->gain = gain;
	a->nstages = nstages;
	a->x0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->x1 = (double*)malloc0(a->nstages * sizeof(complex));
	a->x2 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y1 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y2 = (double*)malloc0(a->nstages * sizeof(complex));
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_bqbp(a);
	return a;
}

void destroy_bqbp(BQBP a)
{
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a->y2);
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x2);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
	_aligned_free(a);
}

void flush_bqbp(BQBP a)
{
	int i;
	for (i = 0; i < a->nstages; i++)
	{
		a->x1[2 * i + 0] = a->x2[2 * i + 0] = a->y1[2 * i + 0] = a->y2[2 * i + 0] = 0.0;
		a->x1[2 * i + 1] = a->x2[2 * i + 1] = a->y1[2 * i + 1] = a->y2[2 * i + 1] = 0.0;
	}
}

void xbqbp(BQBP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, j, n;
		for (i = 0; i < a->size; i++)
		{
			for (j = 0; j < 2; j++)
			{
				a->x0[j] = a->gain * a->in[2 * i + j];
				for (n = 0; n < a->nstages; n++)
				{
					if (n > 0) a->x0[2 * n + j] = a->y0[2 * (n - 1) + j];
					a->y0[2 * n + j] = a->a0 * a->x0[2 * n + j]
						+ a->a1 * a->x1[2 * n + j]
						+ a->a2 * a->x2[2 * n + j]
						+ a->b1 * a->y1[2 * n + j]
						+ a->b2 * a->y2[2 * n + j];
					a->y2[2 * n + j] = a->y1[2 * n + j];
					a->y1[2 * n + j] = a->y0[2 * n + j];
					a->x2[2 * n + j] = a->x1[2 * n + j];
					a->x1[2 * n + j] = a->x0[2 * n + j];
				}
				a->out[2 * i + j] = a->y0[2 * (a->nstages - 1) + j];
			}
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(complex));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_bqbp(BQBP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_bqbp(BQBP a, int rate)
{
	a->rate = rate;
	calc_bqbp(a);
}

void setSize_bqbp(BQBP a, int size)
{
	a->size = size;
	flush_bqbp(a);
}

/********************************************************************************************************
*																										*
*									  Double Bi-Quad Band-Pass				     						*
*																										*
********************************************************************************************************/

void calc_dbqbp(BQBP a)
{
	double f0, w0, bw, q, sn, cs, c, den;
	bw = a->f_high - a->f_low;
	f0 = (a->f_high + a->f_low) / 2.0;
	q = f0 / bw;
	w0 = TWOPI * f0 / a->rate;
	sn = sin(w0);
	cs = cos(w0);
	c = sn / (2.0 * q);
	den = 1.0 + c;
	a->a0 = +c / den;
	a->a1 = 0.0;
	a->a2 = -c / den;
	a->b1 = 2.0 * cs / den;
	a->b2 = (c - 1.0) / den;
	flush_dbqbp(a);
}

BQBP create_dbqbp(int run, int size, double* in, double* out, double rate, double f_low, double f_high, double gain, int nstages)
{
	BQBP a = (BQBP)malloc0(sizeof(bqbp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->f_low = f_low;
	a->f_high = f_high;
	a->gain = gain;
	a->nstages = nstages;
	a->x0 = (double*)malloc0(a->nstages * sizeof(double));
	a->x1 = (double*)malloc0(a->nstages * sizeof(double));
	a->x2 = (double*)malloc0(a->nstages * sizeof(double));
	a->y0 = (double*)malloc0(a->nstages * sizeof(double));
	a->y1 = (double*)malloc0(a->nstages * sizeof(double));
	a->y2 = (double*)malloc0(a->nstages * sizeof(double));
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_dbqbp(a);
	return a;
}

void destroy_dbqbp(BQBP a)
{
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a->y2);
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x2);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
	_aligned_free(a);
}

void flush_dbqbp(BQBP a)
{
	int i;
	for (i = 0; i < a->nstages; i++)
	{
		a->x1[i] = a->x2[i] = a->y1[i] = a->y2[i] = 0.0;
	}
}

void xdbqbp(BQBP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, n;
		for (i = 0; i < a->size; i++)
		{
			a->x0[0] = a->gain * a->in[i];
			for (n = 0; n < a->nstages; n++)
			{
				if (n > 0) a->x0[n] = a->y0[n - 1];
				a->y0[n] = a->a0 * a->x0[n]
					+ a->a1 * a->x1[n]
					+ a->a2 * a->x2[n]
					+ a->b1 * a->y1[n]
					+ a->b2 * a->y2[n];
				a->y2[n] = a->y1[n];
				a->y1[n] = a->y0[n];
				a->x2[n] = a->x1[n];
				a->x1[n] = a->x0[n];
			}
			a->out[i] = a->y0[a->nstages - 1];
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(double));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_dbqbp(BQBP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_dbqbp(BQBP a, int rate)
{
	a->rate = rate;
	calc_dbqbp(a);
}

void setSize_dbqbp(BQBP a, int size)
{
	a->size = size;
	flush_dbqbp(a);
}

/********************************************************************************************************
*																										*
*								     Complex Single-Pole High-Pass				     					*
*																										*
********************************************************************************************************/

void calc_sphp(SPHP a)
{
	double g;
	a->x0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->x1 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y0 = (double*)malloc0(a->nstages * sizeof(complex));
	a->y1 = (double*)malloc0(a->nstages * sizeof(complex));
	g = exp(-TWOPI * a->fc / a->rate);
	a->b0 = +0.5 * (1.0 + g);
	a->b1 = -0.5 * (1.0 + g);
	a->a1 = -g;
}

SPHP create_sphp(int run, int size, double* in, double* out, double rate, double fc, int nstages)
{
	SPHP a = (SPHP)malloc0(sizeof(sphp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->fc = fc;
	a->nstages = nstages;
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_sphp(a);
	return a;
}

void decalc_sphp(SPHP a)
{
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
}

void destroy_sphp(SPHP a)
{
	decalc_sphp(a);
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a);
}

void flush_sphp(SPHP a)
{
	memset(a->x0, 0, a->nstages * sizeof(complex));
	memset(a->x1, 0, a->nstages * sizeof(complex));
	memset(a->y0, 0, a->nstages * sizeof(complex));
	memset(a->y1, 0, a->nstages * sizeof(complex));
}

void xsphp(SPHP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, j, n;
		for (i = 0; i < a->size; i++)
		{
			for (j = 0; j < 2; j++)
			{
				a->x0[j] = a->in[2 * i + j];
				for (n = 0; n < a->nstages; n++)
				{
					if (n > 0) a->x0[2 * n + j] = a->y0[2 * (n - 1) + j];
					a->y0[2 * n + j] = a->b0 * a->x0[2 * n + j]
						+ a->b1 * a->x1[2 * n + j]
						- a->a1 * a->y1[2 * n + j];
					a->y1[2 * n + j] = a->y0[2 * n + j];
					a->x1[2 * n + j] = a->x0[2 * n + j];
				}
				a->out[2 * i + j] = a->y0[2 * (a->nstages - 1) + j];
			}
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(complex));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_sphp(SPHP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_sphp(SPHP a, int rate)
{
	decalc_sphp(a);
	a->rate = rate;
	calc_sphp(a);
}

void setSize_sphp(SPHP a, int size)
{
	a->size = size;
	flush_sphp(a);
}

/********************************************************************************************************
*																										*
*								     Double Single-Pole High-Pass				     					*
*																										*
********************************************************************************************************/

void calc_dsphp(SPHP a)
{
	double g;
	a->x0 = (double*)malloc0(a->nstages * sizeof(double));
	a->x1 = (double*)malloc0(a->nstages * sizeof(double));
	a->y0 = (double*)malloc0(a->nstages * sizeof(double));
	a->y1 = (double*)malloc0(a->nstages * sizeof(double));
	g = exp(-TWOPI * a->fc / a->rate);
	a->b0 = +0.5 * (1.0 + g);
	a->b1 = -0.5 * (1.0 + g);
	a->a1 = -g;
}

SPHP create_dsphp(int run, int size, double* in, double* out, double rate, double fc, int nstages)
{
	SPHP a = (SPHP)malloc0(sizeof(sphp));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->rate = rate;
	a->fc = fc;
	a->nstages = nstages;
	InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
	calc_dsphp(a);
	return a;
}

void decalc_dsphp(SPHP a)
{
	_aligned_free(a->y1);
	_aligned_free(a->y0);
	_aligned_free(a->x1);
	_aligned_free(a->x0);
}

void destroy_dsphp(SPHP a)
{
	decalc_dsphp(a);
	DeleteCriticalSection(&a->cs_update);
	_aligned_free(a);
}

void flush_dsphp(SPHP a)
{
	memset(a->x0, 0, a->nstages * sizeof(double));
	memset(a->x1, 0, a->nstages * sizeof(double));
	memset(a->y0, 0, a->nstages * sizeof(double));
	memset(a->y1, 0, a->nstages * sizeof(double));
}

void xdsphp(SPHP a)
{
	EnterCriticalSection(&a->cs_update);
	if (a->run)
	{
		int i, n;
		for (i = 0; i < a->size; i++)
		{
			a->x0[0] = a->in[i];
			for (n = 0; n < a->nstages; n++)
			{
				if (n > 0) a->x0[n] = a->y0[n - 1];
				a->y0[n] = a->b0 * a->x0[n]
					+ a->b1 * a->x1[n]
					- a->a1 * a->y1[n];
				a->y1[n] = a->y0[n];
				a->x1[n] = a->x0[n];
			}
			a->out[i] = a->y0[a->nstages - 1];
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(double));
	LeaveCriticalSection(&a->cs_update);
}

void setBuffers_dsphp(SPHP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_dsphp(SPHP a, int rate)
{
	decalc_dsphp(a);
	a->rate = rate;
	calc_dsphp(a);
}

void setSize_dsphp(SPHP a, int size)
{
	a->size = size;
	flush_dsphp(a);
}