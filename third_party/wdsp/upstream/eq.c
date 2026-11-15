/*  eq.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2017, 2025, 2026 Warren Pratt, NR0V

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

int fEQcompare (const void * a, const void * b)
{
	if (*(double*)a < *(double*)b)
		return -1;
	else if (*(double*)a == *(double*)b)
		return 0;
	else
		return 1;
}

typedef struct _eqimp
{
	int nfreqs;
	int nc;
	int wintype;
	int max_freqs;
	double* fp;
	double* gp;
	double* A;
	double* sary;
	NURBS pnurbs;
	FSAMP pfsamp;
} eqimp, *EQIMP;

EQIMP create_eqimp(int nfreqs, int nc, int wintype, int max_freqs)
{
	EQIMP a = (EQIMP)malloc0(sizeof(eqimp));
	a->nfreqs = nfreqs;
	a->nc = nc;
	a->wintype = wintype;
	a->max_freqs = max_freqs;
	a->fp = (double*)malloc0((a->max_freqs + 2) * sizeof(double));
	a->gp = (double*)malloc0((a->max_freqs + 2) * sizeof(double));
	a->A  = (double*)malloc0((a->nc / 2 + 1) * sizeof(double));
	a->sary = (double*)malloc0(2 * a->max_freqs * sizeof(double));
	a->pnurbs = create_nurbs(nfreqs - 1, 3, 0, 0, 1024,
		EQ_MAXIMUM_CONTROL_POINTS,
		EQ_MAXIMUM_DEGREE,
		EQ_MAXIMUM_U_VALUES,
		EQ_MAXIMUM_FPTS);
	a->pfsamp = create_fsamp(a->nc, a->wintype);
	return a;
}

void destroy_eqimp(EQIMP a)
{
	destroy_fsamp(a->pfsamp);
	destroy_nurbs(a->pnurbs);
	_aligned_free(a->sary);
	_aligned_free(a->A);
	_aligned_free(a->gp);
	_aligned_free(a->fp);
	_aligned_free(a);
}

void setWintype_eqimp (EQIMP a, int wintype)
{
	destroy_fsamp (a->pfsamp);
	a->wintype = wintype;
	a->pfsamp  = create_fsamp (a->nc, wintype);
}

#ifndef M_LN2_10
#define M_LN2_10 3.32192809488736234787
#endif

void eq_impulse (EQIMP a, int N, int nfreqs, double* F, double* G, 
	double samplerate, double scale, int ctfmode, int wintype, int deg, 
	double* impulse)
{
	NURBS pnurbs = a->pnurbs;
	FSAMP pfsamp = a->pfsamp;
	double* fp = a->fp;
	double* gp = a->gp;
	double* A  = a->A;
	double* sary = a->sary;
	double gpreamp, f, frac;
	int i, j, k;
	int mid, low, high;
	fp[0] = 0.0;
	fp[nfreqs + 1] = 1.0;
	gpreamp = G[0];
	for (i = 1; i <= nfreqs; i++)
	{
		fp[i] = 2.0 * F[i] / samplerate;
		if (fp[i] < 0.0) fp[i] = 0.0;
		if (fp[i] > 1.0) fp[i] = 1.0;
		gp[i] = G[i];
	}
	for (i = 1, j = 0; i <= nfreqs; i++, j+=2)
	{
		sary[j + 0] = fp[i];
		sary[j + 1] = gp[i];
	}
	qsort (sary, nfreqs, 2 * sizeof (double), fEQcompare);
	for (i = 1, j = 0; i <= nfreqs; i++, j+=2)
	{
		fp[i] = sary[j + 0];
		gp[i] = sary[j + 1];
	}
	gp[0] = gp[1];
	gp[nfreqs + 1] = gp[nfreqs];
	mid = N / 2;
	if (N & 1)
	{
		low = (int)(fp[1] * mid);
		high = (int)(fp[nfreqs] * mid + 0.5);
	}
	else
	{
		low = (int)(fp[1] * mid - 0.5);
		high = (int)(fp[nfreqs] * mid - 0.5);
	}
	if (deg == 0)	// OLD linear method (degree = 1)
	{
		j = 0;
		if (N & 1)
		{
			for (i = 0; i <= mid; i++)
			{
				f = (double)i / (double)mid;
				while (f > fp[j + 1]) j++;
				frac = (f - fp[j]) / (fp[j + 1] - fp[j]);
				A[i] = exp2 (M_LN2_10 * 0.05 * (frac * gp[j + 1] + (1.0 - frac) * gp[j] + gpreamp)) * scale;
			}
		}
		else
		{
			for (i = 0; i < mid; i++)
			{
				f = ((double)i + 0.5) / (double)mid;
				while (f > fp[j + 1]) j++;
				frac = (f - fp[j]) / (fp[j + 1] - fp[j]);
				A[i] = exp2 (M_LN2_10 * 0.05 * (frac * gp[j + 1] + (1.0 - frac) * gp[j] + gpreamp)) * scale;
			}
		}
	}
	else			// NEW NURBS curves (degrees 1 - 16)
	{
		if (pnurbs != NULL)
		{	
			pnurbs->n = nfreqs - 1;
			pnurbs->p = deg;
			pnurbs->fpts = high - low + 1;
			if (pnurbs->n    >= EQ_MAXIMUM_CONTROL_POINTS  ||
				pnurbs->p    >  EQ_MAXIMUM_DEGREE          ||
				pnurbs->upts >  EQ_MAXIMUM_U_VALUES        ||
				pnurbs->fpts >  EQ_MAXIMUM_FPTS)           return;
			for (i = 0, j = 1; j <= nfreqs; i += 2, j++)
			{
				pnurbs->CP[i + 0] = fp[j];
				pnurbs->CP[i + 1] = gp[j];
			}
			BuildSpline(pnurbs->n, pnurbs->p, pnurbs->r, pnurbs->umethod, pnurbs->U, pnurbs->CP, pnurbs->W, pnurbs->upts,
				pnurbs->Xs, pnurbs->Ys, pnurbs->Uout, pnurbs->fpts, pnurbs->Xf, pnurbs->Yf);
			for (i = low, j = 0; i <= high; i++, j++)
			{
				A[i] = exp2 (M_LN2_10 * 0.05 * (pnurbs->Yf[j] + gpreamp)) * scale;
			}
		}
	}
	switch (ctfmode)
	{
		double lowmag, highmag, flow4, fhigh4;
		case 0:			// only option currently coded
		default:
			if (N & 1)
			{
				lowmag = A[low];
				highmag = A[high];
				flow4 = pow ((double)low / (double)mid, 4.0);
				fhigh4 = pow ((double)high / (double)mid, 4.0);
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
				lowmag = A[low];
				highmag = A[high];
				flow4 = pow ((double)low / (double)mid, 4.0);
				fhigh4 = pow ((double)high / (double)mid, 4.0);
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
			break;
	}
	fsamp_exec (pfsamp, A, impulse, 1, 1.0);
	// print_impulse("eq.txt", N, impulse, 1, 0);
	return;
}


/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Equalizer									*
*																										*
********************************************************************************************************/

EQP create_eqp (int run, int size, int nc, int mp, double *in, double *out, 
	int nfreqs, double* F, double* G, int ctfmode, int wintype, int samplerate)
{
	// NOTE:  'nc' must be >= 'size', both powers-of-two
	EQP a = (EQP) malloc0 (sizeof (eqp));
	a->run = run;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->in = in;
	a->out = out;
	a->nfreqs = nfreqs;
	a->max_freqs = EQ_MAXIMUM_CONTROL_POINTS;
	a->F = (double *) malloc0 ((a->max_freqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->max_freqs + 1) * sizeof (double));
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	a->ctfmode = ctfmode;
	a->wintype = wintype;
	a->samplerate = (double)samplerate;
	a->deg = 0;
	InitializeCriticalSection (&a->csEQ);
	a->peqimp = create_eqimp(a->nfreqs, a->nc, a->wintype, a->max_freqs);
	a->impulse = (double*) malloc0 (a->nc * sizeof (complex));
	eq_impulse (a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate, 
		1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, 
		a->impulse);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, a->mp, 4, a->impulse);
	return a;
}

void destroy_eqp (EQP a)
{
	destroy_fircore (a->p);
	_aligned_free (a->impulse);
	destroy_eqimp(a->peqimp);
	DeleteCriticalSection (&a->csEQ);
	if (a->G) _aligned_free (a->G);
	if (a->F) _aligned_free (a->F);
	_aligned_free (a);
}

void flush_eqp (EQP a)
{
	flush_fircore (a->p);
}

void xeqp (EQP a)
{
	if (a->run)
		xfircore (a->p);
	else
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_eqp (EQP a, double* in, double* out)
{
	EnterCriticalSection (&a->csEQ);
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
	LeaveCriticalSection (&a->csEQ);
}

void setSamplerate_eqp (EQP a, int rate)
{
	EnterCriticalSection (&a->csEQ);
	a->samplerate = rate;
	eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
		1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, 
		a->impulse);
	setImpulse_fircore (a->p, a->impulse, 1);
	LeaveCriticalSection (&a->csEQ);
}

void setSize_eqp (EQP a, int size)
{
	EnterCriticalSection (&a->csEQ);
	a->size = size;
	setSize_fircore (a->p, a->size);
	eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
		1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, 
		a->impulse);
	setImpulse_fircore (a->p, a->impulse, 1);
	LeaveCriticalSection (&a->csEQ);
}

/********************************************************************************************************
*																										*
*							Partitioned Overlap-Save Equalizer:  RXA Properties							*
*																										*
********************************************************************************************************/

PORT
void SetRXAEQRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].eqp.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAEQNC (int channel, int nc)
{
// ****** NOW AUTOMATICALLY SET TO REQUIRED VALUE - DEPRECATED ******
// 	   (Defined for optimum performance.  Not an operator choice.)
	//EQP a = rxa[channel].eqp.p;
	//NURBS b = a->pn;
	//EnterCriticalSection (&ch[channel].csDSP);
	//if (a->nc != nc)
	//{
	//	//a->nc = nc;
	//	a->nc = EQ_MAXIMUM_COEFS;
	//	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	//	{
	//		destroy_fsamp (a->pfsamp);
	//		a->pfsamp = create_fsamp(a->nc, a->wintype);
	//		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
	//			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, 
	//			a->pn, a->pfsamp, a->impulse);
	//		setNc_fircore (a->p, a->nc, a->impulse);
	//	}
	//}
	//LeaveCriticalSection (&ch[channel].csDSP);
	return;
}

PORT
void SetRXAEQMP (int channel, int mp)
{
// ****** NOW AUTOMATICALLY SET TO REQUIRED VALUE - DEPRECATED ******
// 	   (Defined for optimum performance.  Not an operator choice.)
	//EQP a = rxa[channel].eqp.p;
	//EnterCriticalSection (&ch[channel].csDSP);
	//if (a->mp != mp)
	//{
	//	a->mp = mp;
	//	setMp_fircore (a->p, a->mp);
	//}
	//LeaveCriticalSection (&ch[channel].csDSP);
	return;
}

PORT
void SetRXAEQProfile (int channel, int nfreqs, double* F, double* G)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = nfreqs;
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetRXAEQCtfmode (int channel, int mode)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->ctfmode = mode;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetRXAEQWintype (int channel, int wintype)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&ch[channel].csDSP);
	EnterCriticalSection (&a->csEQ);
	a->wintype = wintype;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		setWintype_eqimp (a->peqimp, wintype);
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGrphEQ (int channel, int *rxeq)
{	// three band equalizer (legacy compatibility)
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = 4;
	a->F[1] =  150.0;
	a->F[2] =  400.0;
	a->F[3] = 1500.0;
	a->F[4] = 6000.0;
	a->G[0] = (double)rxeq[0];
	a->G[1] = (double)rxeq[1];
	a->G[2] = (double)rxeq[1];
	a->G[3] = (double)rxeq[2];
	a->G[4] = (double)rxeq[3];
	a->ctfmode = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetRXAGrphEQ10 (int channel, int *rxeq)
{	// ten band equalizer (legacy compatibility)
	int i;
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = 10;
	a->F[1]  =    32.0;
	a->F[2]  =    63.0;
	a->F[3]  =   125.0;
	a->F[4]  =   250.0;
	a->F[5]  =   500.0;
	a->F[6]  =  1000.0;
	a->F[7]  =  2000.0;
	a->F[8]  =  4000.0;
	a->F[9]  =  8000.0;
	a->F[10] = 16000.0;
	for (i = 0; i <= a->nfreqs; i++)
		a->G[i] = (double)rxeq[i];
	a->ctfmode = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		// if(channel == 0) print_impulse ("rxeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetRXAEQCurve (int channel, int deg, int r, int umethod)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->deg = deg;
	b->p = deg;
	b->r = r;
	if (umethod == 0 || umethod == 1) b->umethod = umethod;
	else b->umethod = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		// if(channel == 0) print_impulse ("rxeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetRXAEQWeights (int channel, int nfreq, double* weights)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	for (int i = 0; i < nfreq; i++)
		b->W[i] = weights[i];
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		// if (channel == 0) print_impulse ("rxeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void GetRXAEQDraw (int channel, double* X, double* Y)
{
	EQP a = rxa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	memcpy (X, b->Xs, b->upts * sizeof(double));
	memcpy (Y, b->Ys, b->upts * sizeof(double));
	LeaveCriticalSection (&a->csEQ);
}

/********************************************************************************************************
*																										*
*							Partitioned Overlap-Save Equalizer:  TXA Properties							*
*																										*
********************************************************************************************************/

PORT
void SetTXAEQRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].eqp.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAEQNC (int channel, int nc)
{
// ****** NOW AUTOMATICALLY SET TO REQUIRED VALUE - DEPRECATED ******
// 	   (Defined for optimum performance.  Not an operator choice.)
	//EQP a = txa[channel].eqp.p;
	//NURBS b = a->pn;
	//EnterCriticalSection (&ch[channel].csDSP);
	//if (a->nc != nc)
	//{
	//	a->nc = nc;
	//	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	//	{
	//		destroy_fsamp(a->pfsamp);
	//		a->pfsamp = create_fsamp(a->nc, a->wintype);
	//		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
	//			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->pn, a->pfsamp, a->impulse);
	//		setNc_fircore (a->p, a->nc, a->impulse);
	//	}
	//}
	//LeaveCriticalSection (&ch[channel].csDSP);
	return;
}

PORT
void SetTXAEQMP (int channel, int mp)
{
// ****** NOW AUTOMATICALLY SET TO REQUIRED VALUE - DEPRECATED ******
// 	   (Defined for optimum performance.  Not an operator choice.)
	//EQP a = txa[channel].eqp.p;
	//EnterCriticalSection (&ch[channel].csDSP);
	//if (a->mp != mp)
	//{
	//	a->mp = mp;
	//	setMp_fircore (a->p, a->mp);
	//}
	//LeaveCriticalSection (&ch[channel].csDSP);
	return;
}

PORT
void SetTXAEQProfile (int channel, int nfreqs, double* F, double* G)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = nfreqs;
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetTXAEQCtfmode (int channel, int mode)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->ctfmode = mode;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetTXAEQWintype (int channel, int wintype)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&ch[channel].csDSP);
	EnterCriticalSection (&a->csEQ);
	a->wintype = wintype;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		setWintype_eqimp (a->peqimp, wintype);
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAGrphEQ (int channel, int *txeq)
{	// three band equalizer (legacy compatibility)
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = 4;
	a->F[1] =  150.0;
	a->F[2] =  400.0;
	a->F[3] = 1500.0;
	a->F[4] = 6000.0;
	a->G[0] = (double)txeq[0];
	a->G[1] = (double)txeq[1];
	a->G[2] = (double)txeq[1];
	a->G[3] = (double)txeq[2];
	a->G[4] = (double)txeq[3];
	a->ctfmode = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetTXAGrphEQ10 (int channel, int *txeq)
{	// ten band equalizer (legacy compatibility)
	int i;
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->nfreqs = 10;
	a->F[1]  =    32.0;
	a->F[2]  =    63.0;
	a->F[3]  =   125.0;
	a->F[4]  =   250.0;
	a->F[5]  =   500.0;
	a->F[6]  =  1000.0;
	a->F[7]  =  2000.0;
	a->F[8]  =  4000.0;
	a->F[9]  =  8000.0;
	a->F[10] = 16000.0;
	for (i = 0; i <= a->nfreqs; i++)
		a->G[i] = (double)txeq[i];
	a->ctfmode = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		// print_impulse ("txeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetTXAEQCurve (int channel, int deg, int r, int umethod)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	a->deg = deg;
	b->p = deg;
	b->r = r;
	if (umethod == 0 || umethod == 1) b->umethod = umethod;
	else b->umethod = 0;
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		// print_impulse ("txeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void SetTXAEQWeights (int channel, int nfreq, double* weights)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	for (int i = 0; i < nfreq; i++)
		b->W[i] = weights[i];
	if (!checkSplineInputs (a->nfreqs, b->p, b->r, b->umethod, b->W))
	{
		eq_impulse(a->peqimp, a->nc, a->nfreqs, a->F, a->G, a->samplerate,
			1.0 / (2.0 * a->size), a->ctfmode, a->wintype, a->deg, a->impulse);
		//print_impulse ("txeq.txt", a->nc, impulse, 1, 0);
		setImpulse_fircore (a->p, a->impulse, 1);
	}
	LeaveCriticalSection (&a->csEQ);
}

PORT
void GetTXAEQDraw (int channel, double* X, double* Y)
{
	EQP a = txa[channel].eqp.p;
	NURBS b = a->peqimp->pnurbs;
	EnterCriticalSection (&a->csEQ);
	memcpy(X, b->Xs, b->upts * sizeof(double));
	memcpy(Y, b->Ys, b->upts * sizeof(double));
	LeaveCriticalSection (&a->csEQ);
}

/********************************************************************************************************
*																										*
*											Overlap-Save Equalizer										*
*																										*
********************************************************************************************************/


double* eq_mults (EQIMP peqimp, int size, int nfreqs, double* F, double* G, double samplerate, 
	double scale, int ctfmode, int wintype, int deg, double* impulse)
{
	eq_impulse (peqimp, size, nfreqs, F, G, samplerate, scale, ctfmode, wintype, deg, impulse);
	double* mults = fftcv_mults (2 * size, impulse);
	return mults;
}

void calc_eq (EQ a)
{
	a->scale = 1.0 / (double)(2 * a->size);
	a->infilt = (double *)malloc0 (2 * a->size * sizeof(complex));
	a->product = (double *)malloc0 (2 * a->size * sizeof(complex));
	a->CFor = fftw_plan_dft_1d (2 * a->size, (fftw_complex *)a->infilt, (fftw_complex *)a->product, FFTW_FORWARD, FFTW_PATIENT);
	a->CRev = fftw_plan_dft_1d (2 * a->size, (fftw_complex *)a->product, (fftw_complex *)a->out, FFTW_BACKWARD, FFTW_PATIENT);
	a->mults = eq_mults (a->peqimp, a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype, a->deg,
		a->impulse);
}

void decalc_eq (EQ a)
{
	fftw_destroy_plan (a->CRev);
	fftw_destroy_plan (a->CFor);
	_aligned_free (a->mults);
	_aligned_free (a->product);
	_aligned_free (a->infilt);
}

EQ create_eq (int run, int size, double *in, double *out, int nfreqs, 
	double* F, double* G, int ctfmode, int wintype, int samplerate)
{
	EQ a = (EQ) malloc0 (sizeof (eq));
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->nfreqs = nfreqs;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	a->ctfmode = ctfmode;
	a->wintype = wintype;
	a->samplerate = (double)samplerate;
	a->deg = 0;
	a->peqimp = create_eqimp(a->nfreqs, a->size, a->wintype, EQ_MAXIMUM_CONTROL_POINTS);
	a->impulse = (double*)malloc0((a->size + 1) * sizeof(complex));
	calc_eq (a);
	return a;
}

void destroy_eq (EQ a)
{
	decalc_eq (a);
	_aligned_free(a->impulse);
	destroy_eqimp(a->peqimp);
	_aligned_free (a->G);
	_aligned_free (a->F);
	_aligned_free (a);
}

void flush_eq (EQ a)
{
	memset (a->infilt, 0, 2 * a->size * sizeof (complex));
}

void xeq (EQ a)
{
	int i;
	double I, Q;
	if (a->run)
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

void setBuffers_eq (EQ a, double* in, double* out)
{
	decalc_eq (a);
	a->in = in;
	a->out = out;
	calc_eq (a);
}

void setSamplerate_eq (EQ a, int rate)
{
	decalc_eq (a);
	a->samplerate = rate;
	calc_eq (a);
}

void setSize_eq (EQ a, int size)
{
	decalc_eq (a);
	a->size = size;
	calc_eq (a);
}

/********************************************************************************************************
*																										*
*								Overlap-Save Equalizer:  RXA Properties									*
*																										*
********************************************************************************************************/
/*  // UNCOMMENT properties when a pointer is in place in rxa[channel]
PORT
void SetRXAEQRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].eq.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAEQProfile (int channel, int nfreqs, double* F, double* G)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = nfreqs;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAEQCtfmode (int channel, int mode)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].eq.p;
	a->ctfmode = mode;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAEQWintype (int channel, int wintype)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].eq.p;
	a->wintype = wintype;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGrphEQ (int channel, int *rxeq)
{	// three band equalizer (legacy compatibility)
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = 4;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->F[1] =  150.0;
	a->F[2] =  400.0;
	a->F[3] = 1500.0;
	a->F[4] = 6000.0;
	a->G[0] = (double)rxeq[0];
	a->G[1] = (double)rxeq[1];
	a->G[2] = (double)rxeq[1];
	a->G[3] = (double)rxeq[2];
	a->G[4] = (double)rxeq[3];
	a->ctfmode = 0;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAGrphEQ10 (int channel, int *rxeq)
{	// ten band equalizer (legacy compatibility)
	EQ a;
	int i;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = 10;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->F[1]  =    32.0;
	a->F[2]  =    63.0;
	a->F[3]  =   125.0;
	a->F[4]  =   250.0;
	a->F[5]  =   500.0;
	a->F[6]  =  1000.0;
	a->F[7]  =  2000.0;
	a->F[8]  =  4000.0;
	a->F[9]  =  8000.0;
	a->F[10] = 16000.0;
	for (i = 0; i <= a->nfreqs; i++)
		a->G[i] = (double)rxeq[i];
	a->ctfmode = 0;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}
*/
/********************************************************************************************************
*																										*
*								Overlap-Save Equalizer:  TXA Properties									*
*																										*
********************************************************************************************************/
/*  // UNCOMMENT properties when a pointer is in place in rxa[channel]
PORT
void SetTXAEQRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].eq.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAEQProfile (int channel, int nfreqs, double* F, double* G)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = nfreqs;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	memcpy (a->F, F, (nfreqs + 1) * sizeof (double));
	memcpy (a->G, G, (nfreqs + 1) * sizeof (double));
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAEQCtfmode (int channel, int mode)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].eq.p;
	a->ctfmode = mode;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAEQMethod (int channel, int wintype)
{
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].eq.p;
	a->wintype = wintype;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAGrphEQ (int channel, int *txeq)
{	// three band equalizer (legacy compatibility)
	EQ a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = 4;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->F[1] =  150.0;
	a->F[2] =  400.0;
	a->F[3] = 1500.0;
	a->F[4] = 6000.0;
	a->G[0] = (double)txeq[0];
	a->G[1] = (double)txeq[1];
	a->G[2] = (double)txeq[1];
	a->G[3] = (double)txeq[2];
	a->G[4] = (double)txeq[3];
	a->ctfmode = 0;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAGrphEQ10 (int channel, int *txeq)
{	// ten band equalizer (legacy compatibility)
	EQ a;
	int i;
	EnterCriticalSection (&ch[channel].csDSP);
	a = txa[channel].eq.p;
	_aligned_free (a->G);
	_aligned_free (a->F);
	a->nfreqs = 10;
	a->F = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->G = (double *) malloc0 ((a->nfreqs + 1) * sizeof (double));
	a->F[1]  =    32.0;
	a->F[2]  =    63.0;
	a->F[3]  =   125.0;
	a->F[4]  =   250.0;
	a->F[5]  =   500.0;
	a->F[6]  =  1000.0;
	a->F[7]  =  2000.0;
	a->F[8]  =  4000.0;
	a->F[9]  =  8000.0;
	a->F[10] = 16000.0;
	for (i = 0; i <= a->nfreqs; i++)
		a->G[i] = (double)txeq[i];
	a->ctfmode = 0;
	_aligned_free (a->mults);
	a->mults = eq_mults (a->size, a->nfreqs, a->F, a->G, a->samplerate, a->scale, a->ctfmode, a->wintype);
	LeaveCriticalSection (&ch[channel].csDSP);
}
*/