/*  cfir.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014, 2016, 2021, 2026 Warren Pratt, NR0V

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
#define _CRT_SECURE_NO_DEPRECATE
#include "comm.h"

#include "firmin.h"

typedef struct _cfirimp
{
	int nc;
	int wintype;
	double* A;
	double* xistion;
	double* impulse;
	FSAMP pfsamp;
} cfirimp, * CFIRIMP;

typedef struct _cfir
{
	int run;
	int size;
	int nc;
	int mp;
	double* in;
	double* out;
	int runrate;
	int cicrate;
	int DD;
	int R;
	int Pairs;
	double cutoff;
	double scale;
	int xtype;
	double xbw;
	int wintype;
	CFIRIMP pcfirimp;
	FIRCORE p;
} cfir, * CFIR;

void cfir_impulse(CFIRIMP a, int N, int DD, int R, int Pairs, double runrate, double cicrate,
	double cutoff, int xtype, double xbw, int rtype, double scale, int wintype);

CFIRIMP build_cfir_impulse(int nc, int wintype)
{
	CFIRIMP s = (CFIRIMP)malloc0(sizeof(cfirimp));
	s->nc = nc;
	s->wintype = wintype;
	s->A       = (double*)malloc0 (nc * sizeof(double));
	s->xistion = (double*)malloc0 (nc * sizeof(double));
	s->impulse = (double*)malloc0 (nc * sizeof(complex));
	s->pfsamp = create_fsamp (s->nc, s->wintype);
	return s;
}

void teardown_cfir_impulse(CFIRIMP s)
{
	destroy_fsamp(s->pfsamp);
	_aligned_free(s->impulse);
	_aligned_free(s->xistion);
	_aligned_free(s->A);
	_aligned_free(s);
}

void calc_cfir (CFIR a)
{
	a->pcfirimp = build_cfir_impulse(a->nc, a->wintype);
	a->scale = 1.0 / (double)(2 * a->size);
	cfir_impulse (a->pcfirimp, a->nc, a->DD, a->R, a->Pairs, a->runrate, a->cicrate, 
		a->cutoff, a->xtype, a->xbw, 1, a->scale, a->wintype);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, a->mp, 16, a->pcfirimp->impulse);
}

void decalc_cfir (CFIR a)
{
	destroy_fircore (a->p);
	teardown_cfir_impulse(a->pcfirimp);
}

CFIR create_cfir (int run, int size, int nc, int mp, double* in, double* out, int runrate, int cicrate, 
	int DD, int R, int Pairs, double cutoff, int xtype, double xbw, int wintype)
//	run:  0 - no action; 1 - operate
//	size:  number of complex samples in an input buffer to the CFIR filter
//	nc:  number of filter coefficients
//  mp:  minimum phase flag
//	in:  pointer to the input buffer
//	out:  pointer to the output buffer
//	rate:  samplerate
//	DD:  differential delay of the CIC to be compensated (usually 1 or 2)
//	R:  interpolation factor of CIC
//	Pairs:  number of comb-integrator pairs in the CIC
//	cutoff:  cutoff frequency
//  xtype:  0 - fourth power transition; 1 - raised cosine transition; 2 - brick wall
//  xbw:  width of raised cosine transition
{
	CFIR a = (CFIR) malloc0 (sizeof (cfir));
	a->run = run;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->in = in;
	a->out = out;
	a->runrate = runrate;
	a->cicrate = cicrate;
	a->DD = DD;
	a->R = R;
	a->Pairs = Pairs;
	a->cutoff = cutoff;
	a->xtype = xtype;
	a->xbw = xbw;
	a->wintype = wintype;
	calc_cfir (a);
	return a;
}

void destroy_cfir (CFIR a)
{
	decalc_cfir (a);
	_aligned_free (a);
}

void flush_cfir (CFIR a)
{
	flush_fircore (a->p);
}

void xcfir (CFIR a)
{
	if (a->run)
		xfircore (a->p);
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_cfir (CFIR a, double* in, double* out)
{
	decalc_cfir (a);
	a->in = in;
	a->out = out;
	calc_cfir (a);
}

void setSamplerate_cfir (CFIR a, int rate)
{
	decalc_cfir (a);
	a->runrate = rate;
	calc_cfir (a);
}

void setSize_cfir (CFIR a, int size)
{
	decalc_cfir (a);
	a->size = size;
	calc_cfir (a);
}

void setOutRate_cfir (CFIR a, int rate)
{
	decalc_cfir (a);
	a->cicrate = rate;
	calc_cfir (a);
}

void cfir_impulse (CFIRIMP a, int N, int DD, int R, int Pairs, double runrate, double cicrate, 
	double cutoff, int xtype, double xbw, int rtype, double scale, int wintype)
{
	// N:		number of impulse response samples
	// DD:		differential delay used in the CIC filter
	// R:		interpolation / decimation factor of the CIC
	// Pairs:	number of comb-integrator pairs in the CIC
	// runrate:	sample rate at which this filter is to run (assumes there may be flat interp. between this filter and the CIC)
	// cicrate: sample rate at interface to CIC
	// cutoff:	cutoff frequency
	// xtype:	transition type, 0 for 4th-power rolloff, 1 for raised cosine, 2 for brick wall
	// xbw:		transition bandwidth for raised cosine
	// rtype:	0 for real output, 1 for complex output
	// scale:	scale factor to be applied to the output
	int i, j;
	double tmp, local_scale, ri, fn, mag = 1.0;
	double ft = cutoff / cicrate;
	int u_samps = (N + 1) / 2;
	int c_samps = (int)(cutoff / runrate * N) + (N + 1) / 2 - N / 2;
	int x_samps = (int)(xbw / runrate * N);
	double offset = 0.5 - 0.5 * (double)((N + 1) / 2 - N / 2);
	double delta = PI / (double)x_samps;
	double L = cicrate / runrate;
	double phs = 0.0;
	for (i = 0; i <= x_samps; i++)
	{
		a->xistion[i] = 0.5 * (cos (phs) + 1.0);
		phs += delta;
	}
	if ((tmp = DD * R * sin (PI * ft / R) / sin (PI * DD * ft)) < 0.0)
		tmp = -tmp;
	local_scale = scale / pow (tmp, Pairs);
	if (xtype == 0)
	{
		for (i = 0, ri = offset; i < u_samps; i++, ri += 1.0)
		{
			fn = ri / (L * (double)N);
			if (fn <= ft)
			{
				if (fn == 0.0) tmp = 1.0;
				else if ((tmp = DD * R * sin (PI * fn / R) / sin (PI * DD * fn)) < 0.0)
					tmp = -tmp;
				mag = pow (tmp, Pairs) * local_scale;
			}
			else
				mag *= (ft * ft * ft * ft) / (fn * fn * fn * fn);
			a->A[i] = mag;
		}
	}
	else if (xtype == 1)
	{
		for (i = 0, ri = offset; i < u_samps; i++, ri += 1.0)
		{
			fn = ri / (L *(double)N);
			if (i < c_samps)
			{
				if (fn == 0.0) tmp = 1.0;
				else if ((tmp = DD * R * sin (PI * fn / R) / sin (PI * DD * fn)) < 0.0)
					tmp = -tmp;
				mag = pow (tmp, Pairs) * local_scale;
				a->A[i] = mag;
			}
			else if ( i >= c_samps && i <= c_samps + x_samps)
				a->A[i] = mag * a->xistion[i - c_samps];
			else
				a->A[i] = 0.0;
		}
	}
	else if (xtype == 2)
	{
		for (i = 0, ri = offset; i < u_samps; i++, ri += 1.0)
		{
			fn = ri / (L * (double)N);
			if (fn <= ft)
			{
				if (fn == 0.0) tmp = 1.0;
				else if ((tmp = DD * R * sin(PI * fn / R) / sin(PI * DD * fn)) < 0.0)
					tmp = -tmp;
				mag = pow (tmp, Pairs) * local_scale;
			}
			else
				mag = 0.0;
			a->A[i] = mag;
		}
	}
	if (N & 1)
		for (i = u_samps, j = 2; i < N; i++, j++)
			a->A[i] = a->A[u_samps - j];
	else
		for (i = u_samps, j = 1; i < N; i++, j++)
			a->A[i] = a->A[u_samps - j];
	fsamp_exec (a->pfsamp, a->A, a->impulse, rtype, 1.0);
	// print_impulse ("cfirImpulse.txt", N, a->impulse, 1, 0);
}

/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetTXACFIRRun (int channel, int run)
{
	EnterCriticalSection(&ch[channel].csDSP);
	txa[channel].cfir.p->run = run;
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXACFIRNC(int channel, int nc)
{
	// NOTE:  'nc' must be >= 'size'
	CFIR a;
	EnterCriticalSection(&ch[channel].csDSP);
	a = txa[channel].cfir.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		decalc_cfir(a);
		calc_cfir(a);
	}
	LeaveCriticalSection(&ch[channel].csDSP);
}

PORT
void SetTXACFIRCurve(int channel, int DD, int R, int Pairs, int runrate, int cicrate,
	double cutoff, int xtype, double xbw)
{
	// DD:		differential delay used in the CIC filter
	// R:		interpolation / decimation factor of the CIC
	// Pairs:	number of comb-integrator pairs in the CIC
	// runrate:	sample rate at which this filter is to run (assumes there may be flat interp. between this filter and the CIC)
	// cicrate: sample rate at interface to CIC
	// cutoff:	cutoff frequency
	// xtype:	transition type, 0 for 4th-power rolloff, 1 for raised cosine, 2 for brick wall
	// xbw:		transition bandwidth for raised cosine
	CFIR a = txa[channel].cfir.p;
	CFIRIMP b = a->pcfirimp;
	EnterCriticalSection(&ch[channel].csDSP);
	a->DD = DD;
	a->R = R;
	a->Pairs = Pairs;
	a->runrate = runrate;
	a->cicrate = cicrate;
	a->cutoff = cutoff;
	a->xtype = xtype;
	a->xbw = xbw;
	cfir_impulse (b, a->nc, a->DD, a->R, a->Pairs, a->runrate, a->cicrate,
		a->cutoff, a->xtype, a->xbw, 1, a->scale, a->wintype);
	setImpulse_fircore(a->p, b->impulse, 1);
	LeaveCriticalSection(&ch[channel].csDSP);
}