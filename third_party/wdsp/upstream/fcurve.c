/*  fcurve.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2025 Warren Pratt, NR0V

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

typedef struct _fcimp
{
	int nc;
	int wintype;
	double* A;
	double* impulse;
	FSAMP pfsamp;
}fcimp, *FCIMP;

FCIMP build_fcimp (int nc, int wintype)
{
	FCIMP a = (FCIMP)malloc0(sizeof(fcimp));
	a->nc = nc;
	a->wintype = wintype;
	a->A = (double*)malloc0((nc / 2 + 1) * sizeof(double));
	a->impulse = (double*)malloc0(a->nc * sizeof(complex));
	a->pfsamp = create_fsamp(a->nc, a->wintype);
	return a;
}

void teardown_fcimp (FCIMP a)
{
	destroy_fsamp(a->pfsamp);
	_aligned_free(a->impulse);
	_aligned_free(a->A);
	_aligned_free(a);
}

double* get_pfcpulse(FCIMP a)
{
	return a->impulse;
}

void exec_fcimp (FCIMP a, double f0, double f1, double g0, double g1, 
	int curve, double samplerate, double scale, int ctfmode)
{
	int i;
	double fn, f;
	int mid = a->nc / 2;
	double g0_lin = pow(10.0, g0 / 20.0);
	if (a->nc & 1)
	{
		for (i = 0; i <= mid; i++)
		{
			fn = (double)i / (double)mid;
			f = fn * samplerate / 2.0;
			switch (curve)
			{
			case 0:	// fm pre-emphasis
				if (f0 > 0.0)
					a->A[i] = scale * (g0_lin * f / f0);
				else
					a->A[i] = 0.0;
				break;
			case 1:	// fm de-emphasis
				if (f > 0.0)
					a->A[i] = scale * (g0_lin * f0 / f);
				else
					a->A[i] = 0.0;
				break;
			}
		}
	}
	else
	{
		for (i = 0; i < mid; i++)
		{
			fn = ((double)i + 0.5) / (double)mid;
			f = fn * samplerate / 2.0;
			switch (curve)
			{
			case 0:	// fm pre-emphasis
				if (f0 > 0.0)
					a->A[i] = scale * (g0_lin * f / f0);
				else
					a->A[i] = 0.0;
				break;
			case 1:	// fm de-emphasis
				if (f > 0.0)
					a->A[i] = scale * (g0_lin * f0 / f);
				else
					a->A[i] = 0.0;
				break;
			}
		}
	}
	if (ctfmode == 0)
	{
		int k, low, high;
		double lowmag, highmag, flow4, fhigh4;
		if (a->nc & 1)
		{
			low = (int)(2.0 * f0 / samplerate * mid);
			high = (int)(2.0 * f1 / samplerate * mid + 0.5);
			lowmag = a->A[low];
			highmag = a->A[high];
			flow4 = pow((double)low / (double)mid, 4.0);
			fhigh4 = pow((double)high / (double)mid, 4.0);
			k = low;
			while (--k >= 0)
			{
				f = (double)k / (double)mid;
				lowmag *= (f * f * f * f) / flow4;
				if (lowmag < 1.0e-100) lowmag = 1.0e-100;
				a->A[k] = lowmag;
			}
			k = high;
			while (++k <= mid)
			{
				f = (double)k / (double)mid;
				highmag *= fhigh4 / (f * f * f * f);
				if (highmag < 1.0e-100) highmag = 1.0e-100;
				a->A[k] = highmag;
			}
		}
		else
		{
			low = (int)(2.0 * f0 / samplerate * mid - 0.5);
			high = (int)(2.0 * f1 / samplerate * mid - 0.5);
			lowmag = a->A[low];
			highmag = a->A[high];
			flow4 = pow((double)low / (double)mid, 4.0);
			fhigh4 = pow((double)high / (double)mid, 4.0);
			k = low;
			while (--k >= 0)
			{
				f = (double)k / (double)mid;
				lowmag *= (f * f * f * f) / flow4;
				if (lowmag < 1.0e-100) lowmag = 1.0e-100;
				a->A[k] = lowmag;
			}
			k = high;
			while (++k < mid)
			{
				f = (double)k / (double)mid;
				highmag *= fhigh4 / (f * f * f * f);
				if (highmag < 1.0e-100) highmag = 1.0e-100;
				a->A[k] = highmag;
			}
		}
	}
	fsamp_exec(a->pfsamp, a->A, a->impulse, 1, 1.0);
	// if (samplerate == 48000) print_impulse ("emphr.txt", a->nc, a->impulse, 1, 0);
}

double* fc_impulse (int nc, double f0, double f1, double g0, double g1, int curve, double samplerate, double scale, int ctfmode, int wintype)
{
	// check for previous in the cache
	struct Params 
	{
		int     nc;
		int     curve;
		int     ctfmode;
		int     wintype;
		double  f0;
		double  f1;
		double  g0;
		double  g1;
		double  samplerate;
		double  scale;
	};

	struct Params params;
	memset(&params, 0, sizeof(params));
	params.nc = nc;
	params.curve = curve;
	params.ctfmode = ctfmode;
	params.wintype = wintype;
	params.f0 = f0;
	params.f1 = f1;
	params.g0 = g0;
	params.g1 = g1;
	params.samplerate = samplerate;
	params.scale = scale;

	HASH_T h = fnv1a_hash(&params, sizeof(params));
	double* imp = get_impulse_cache_entry(FC_CACHE, h, nc);
	if (imp) return imp;
	//

	double* A  = (double *) malloc0 ((nc / 2 + 1) * sizeof (double));
	int i;
	double fn, f;
	double* impulse;
	int mid = nc / 2;
	double g0_lin = pow(10.0, g0 / 20.0);
	if (nc & 1)
	{
		for (i = 0; i <= mid; i++)
		{
			fn = (double)i / (double)mid;
			f = fn * samplerate / 2.0;
			switch (curve)
			{
			case 0:	// fm pre-emphasis
				if (f0 > 0.0)
					A[i] = scale * (g0_lin * f / f0);
				else
					A[i] = 0.0;
				break;
			case 1:	// fm de-emphasis
				if (f > 0.0)
					A[i] = scale * (g0_lin * f0 / f);
				else
					A[i] = 0.0;
				break;
			}
		}
	}
	else
	{
		for (i = 0; i < mid; i++)
		{
			fn = ((double)i + 0.5) / (double)mid;
			f = fn * samplerate / 2.0;
			switch (curve)
			{
			case 0:	// fm pre-emphasis
				if (f0 > 0.0)
					A[i] = scale * (g0_lin * f / f0);
				else
					A[i] = 0.0;
				break;
			case 1:	// fm de-emphasis
				if (f > 0.0)
					A[i] = scale * (g0_lin * f0 / f);
				else
					A[i] = 0.0;
				break;
			}
		}
	}
	if (ctfmode == 0)
	{
		int k, low, high;
		double lowmag, highmag, flow4, fhigh4;
		if (nc & 1)
		{
			low  = (int)(2.0 * f0 / samplerate * mid);
			high = (int)(2.0 * f1 / samplerate * mid + 0.5);
			lowmag = A[low];
			highmag = A[high];
			flow4 = pow((double)low / (double)mid, 4.0);
			fhigh4 = pow((double)high / (double)mid, 4.0);
			k = low;
			while (--k >= 0)
			{
				f = (double)k / (double)mid;
				lowmag *= (f * f * f * f) / flow4;
				if (lowmag < 1.0e-100) lowmag = 1.0e-100;
				A[k] = lowmag;
			}
			k = high;
			while (++k <= mid)
			{
				f = (double)k / (double)mid;
				highmag *= fhigh4 / (f * f * f * f);
				if (highmag < 1.0e-100) highmag = 1.0e-100;
				A[k] = highmag;
			}
		}
		else
		{
			low  = (int)(2.0 * f0 / samplerate * mid - 0.5);
			high = (int)(2.0 * f1 / samplerate * mid - 0.5);
			lowmag = A[low];
			highmag = A[high];
			flow4 = pow((double)low / (double)mid, 4.0);
			fhigh4 = pow((double)high / (double)mid, 4.0);
			k = low;
			while (--k >= 0)
			{
				f = (double)k / (double)mid;
				lowmag *= (f * f * f * f) / flow4;
				if (lowmag < 1.0e-100) lowmag = 1.0e-100;
				A[k] = lowmag;
			}
			k = high;
			while (++k < mid)
			{
				f = (double)k / (double)mid;
				highmag *= fhigh4 / (f * f * f * f);
				if (highmag < 1.0e-100) highmag = 1.0e-100;
				A[k] = highmag;
			}
		}
	}
	if (nc & 1)
		impulse = fir_fsamp_odd(nc, A, 1, 1.0, wintype);
	else
		impulse = fir_fsamp(nc, A, 1, 1.0, wintype);
	// print_impulse ("emph.txt", size + 1, impulse, 1, 0);
	_aligned_free (A);

	// store in cache
	add_impulse_to_cache(FC_CACHE, h, nc, impulse);

	return impulse;
}

// generate mask for Overlap-Save Filter
double* fc_mults (int size, double f0, double f1, double g0, double g1, int curve, double samplerate, double scale, int ctfmode, int wintype)
{
	double* impulse = fc_impulse (size + 1, f0, f1, g0, g1, curve, samplerate, scale, ctfmode, wintype);
	double* mults = fftcv_mults(2 * size, impulse);
	_aligned_free (impulse);
	return mults;
}
