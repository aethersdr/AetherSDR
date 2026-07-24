/*  cfcomp.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2017, 2021, 2026 Warren Pratt, NR0V

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

void calc_cfcwindow (CFCOMP a)
{
	int i;
	double arg0, arg1, cgsum, igsum, coherent_gain, inherent_power_gain, wmult;
	switch (a->wintype)
	{
	case 0:
		arg0 = 2.0 * PI / (double)a->fsize;
		cgsum = 0.0;
		igsum = 0.0;
		for (i = 0; i < a->fsize; i++)
		{
			a->window[i] = sqrt (0.54 - 0.46 * cos((double)i * arg0));
			cgsum += a->window[i];
			igsum += a->window[i] * a->window[i];
		}
		coherent_gain = cgsum / (double)a->fsize;
		inherent_power_gain = igsum / (double)a->fsize;
		wmult = 1.0 / sqrt (inherent_power_gain);
		for (i = 0; i < a->fsize; i++)
			a->window[i] *= wmult;
		a->winfudge = sqrt (1.0 / coherent_gain);
		break;
	case 1:
		arg0 = 2.0 * PI / (double)a->fsize;
		cgsum = 0.0;
		igsum = 0.0;
		for (i = 0; i < a->fsize; i++)
		{
			arg1 = cos(arg0 * (double)i);
			a->window[i]  = sqrt   (+0.21747
				          + arg1 * (-0.45325
				          + arg1 * (+0.28256
				          + arg1 * (-0.04672))));
			cgsum += a->window[i];
			igsum += a->window[i] * a->window[i];
		}
		coherent_gain = cgsum / (double)a->fsize;
		inherent_power_gain = igsum / (double)a->fsize;
		wmult = 1.0 / sqrt (inherent_power_gain);
		for (i = 0; i < a->fsize; i++)
			a->window[i] *= wmult;
		a->winfudge = sqrt (1.0 / coherent_gain);
		break;
	}
}

int fCOMPcompare (const void * a, const void * b)
{
	if (*(double*)a < *(double*)b)
		return -1;
	else if (*(double*)a == *(double*)b)
		return 0;
	else
		return 1;
}

#ifndef M_LN2_10
#define M_LN2_10 3.32192809488736234787
#endif

void calc_compG (CFCOMP a)
{
	int i, j;
	double f, frac, fincr, fmax;
	NURBS bg = a->png;
	a->precomplin = pow (10.0, 0.05 * a->precomp);
	fmax = 0.5 * a->rate;
	for (i = 0; i < a->nfreqsG; i++)
	{
		a->Fg[i] = max (a->Fg[i], 0.0);
		a->Fg[i] = min (a->Fg[i], fmax);
		a->G[i] = max (a->G[i], 0.0);
	}
	for (i = 0; i < a->nfreqsG; i++)
	{
		a->saryG[2 * i + 0] = a->Fg[i];
		a->saryG[2 * i + 1] = a->G[i];
	}
	qsort (a->saryG, a->nfreqsG, 2 * sizeof (double), fCOMPcompare);
	for (i = 0; i < a->nfreqsG; i++)
	{
		a->Fg[i] = a->saryG[2 * i + 0];
		a->G[i]  = a->saryG[2 * i + 1];
	}
	a->fpG[0] = 0.0;
	a->fpG[a->nfreqsG + 1] = fmax;
	a->gp[0] = a->G[0];
	a->gp[a->nfreqsG + 1] = a->G[a->nfreqsG - 1];
	for (i = 0, j = 1; i < a->nfreqsG; i++, j++)
	{
		a->fpG[j] = a->Fg[i];
		a->gp[j] = a->G[i];
	}
	fincr = a->rate / (double)a->fsize;
	if (a->gdeg == 0)
	{
		for (i = 0, j = 0; i < a->msize; i++)
		{
			f = fincr * (double)i;
			while (f >= a->fpG[j + 1] && j < a->nfreqsG) j++;
			frac = (f - a->fpG[j]) / (a->fpG[j + 1] - a->fpG[j]);
			a->comp[i] = exp2(M_LN2_10 * 0.05 * (frac * a->gp[j + 1] + (1.0 - frac) * a->gp[j]));
			a->cfc_gain[i] = a->precomplin * a->comp[i];
		}
	}
	else
	{
		int low = 0, high = 0;
		double g_low  = exp2(M_LN2_10 * 0.05 * a->gp[0]);
		double g_high = exp2(M_LN2_10 * 0.05 * a->gp[a->nfreqsG + 1]);
		i = 0;
		f = 0.0;
		while (f <= a->fpG[1])
		{
			a->comp[i] = g_low;
			a->cfc_gain[i] = a->precomplin * a->comp[i];
			f += fincr;
			low = ++i;
		}
		i = a->msize - 1;
		f = a->fpG[a->nfreqsG + 1];
		while (f >= a->fpG[a->nfreqsG])
		{
			a->comp[i] = g_high;
			a->cfc_gain[i] = a->precomplin * a->comp[i];
			f -= fincr;
			high = --i;
		}
		bg->fpts = high - low + 1;
		bg->n = a->nfreqsG - 1;
		bg->p = a->gdeg;
		if (bg->n >= bg->max_cp || bg->p > bg->max_p ||
			bg->upts > bg->max_upts || bg->fpts > bg->max_fpts) return;
		for (i = 0, j = 1; j <= a->nfreqsG; i += 2, j++)
		{
			bg->CP[i + 0] = a->fpG[j];
			bg->CP[i + 1] = a->gp[j];
		}
		BuildSpline (bg->n, bg->p, bg->r, bg->umethod, bg->U, bg->CP, bg->W, bg->upts,
			bg->Xs, bg->Ys, bg->Uout, bg->fpts, bg->Xf, bg->Yf);
		for (i = low, j = 0; i <= high; i++, j++)
		{
			a->comp[i] = exp2(M_LN2_10 * 0.05 * bg->Yf[j]);
			a->cfc_gain[i] = a->precomplin * a->comp[i];
		}
	}
	// print_impulse ("comp.txt", a->msize, a->cfc_gain, 0, 0);
}

void calc_compE(CFCOMP a)
{
	int i, j;
	double f, frac, fincr, fmax;
	NURBS be = a->pne;
	a->prepeqlin = pow(10.0, 0.05 * a->prepeq);
	fmax = 0.5 * a->rate;
	for (i = 0; i < a->nfreqsE; i++)
	{
		a->Fe[i] = max(a->Fe[i], 0.0);
		a->Fe[i] = min(a->Fe[i], fmax);
	}
	for (i = 0; i < a->nfreqsE; i++)
	{
		a->saryE[2 * i + 0] = a->Fe[i];
		a->saryE[2 * i + 1] = a->E[i];
	}
	qsort(a->saryE, a->nfreqsE, 2 * sizeof(double), fCOMPcompare);
	for (i = 0; i < a->nfreqsE; i++)
	{
		a->Fe[i] = a->saryE[2 * i + 0];
		a->E[i]  = a->saryE[2 * i + 1];
	}
	a->fpE[0] = 0.0;
	a->fpE[a->nfreqsE + 1] = fmax;
	a->ep[0] = a->E[0];
	a->ep[a->nfreqsE + 1] = a->E[a->nfreqsE - 1];
	for (i = 0, j = 1; i < a->nfreqsE; i++, j++)
	{
		a->fpE[j] = a->Fe[i];
		a->ep[j] = a->E[i];
	}
	fincr = a->rate / (double)a->fsize;
	if (a->edeg == 0)
	{
		for (i = 0, j = 0; i < a->msize; i++)
		{
			f = fincr * (double)i;
			while (f >= a->fpE[j + 1] && j < a->nfreqsE) j++;
			frac = (f - a->fpE[j]) / (a->fpE[j + 1] - a->fpE[j]);
			a->peq[i] = exp2(M_LN2_10 * 0.05 * (frac * a->ep[j + 1] + (1.0 - frac) * a->ep[j]));
		}
	}
	else
	{
		int low = 0, high = 0;
		double e_low = exp2(M_LN2_10 * 0.05 * a->ep[0]);
		double e_high = exp2(M_LN2_10 * 0.05 * a->ep[a->nfreqsE + 1]);
		i = 0;
		f = 0.0;
		while (f <= a->fpE[1])
		{
			a->peq[i] = e_low;
			f += fincr;
			low = ++i;
		}
		i = a->msize - 1;
		f = a->fpE[a->nfreqsE + 1];
		while (f >= a->fpE[a->nfreqsE])
		{
			a->peq[i] = e_high;
			f -= fincr;
			high = --i;
		}
		be->fpts = high - low + 1;
		be->n = a->nfreqsE - 1;
		be->p = a->edeg;
		if (be->n >= be->max_cp || be->p > be->max_p ||
			be->upts > be->max_upts || be->fpts > be->max_fpts) return;
		for (i = 0, j = 1; j <= a->nfreqsE; i += 2, j++)
		{
			be->CP[i + 0] = a->fpE[j];
			be->CP[i + 1] = a->ep[j];
		}
		BuildSpline(be->n, be->p, be->r, be->umethod, be->U, be->CP, be->W, be->upts,
			be->Xs, be->Ys, be->Uout, be->fpts, be->Xf, be->Yf);
		for (i = low, j = 0; i <= high; i++, j++)
		{
			a->peq[i] = exp2(M_LN2_10 * 0.05 * be->Yf[j]);
		}
	}
	// print_impulse ("compeq.txt", a->msize, a->peq, 0, 0);
}


void calc_cfcomp(CFCOMP a)
{
	int i;
	a->incr = a->fsize / a->ovrlp;
	if (a->fsize > a->bsize)
		a->iasize = a->fsize;
	else
		a->iasize = a->bsize + a->fsize - a->incr;
	a->iainidx = 0;
	a->iaoutidx = 0;
	if (a->fsize > a->bsize)
	{
		if (a->bsize > a->incr)  a->oasize = a->bsize;
		else					 a->oasize = a->incr;
		a->oainidx = (a->fsize - a->bsize - a->incr) % a->oasize;
	}
	else
	{
		a->oasize = a->bsize;
		a->oainidx = a->fsize - a->incr;
	}
	a->init_oainidx = a->oainidx;
	a->oaoutidx = 0;
	a->msize = a->fsize / 2 + 1;
	a->window    = (double *)malloc0 (a->fsize  * sizeof(double));
	a->inaccum   = (double *)malloc0 (a->iasize * sizeof(double));
	a->forfftin  = (double *)malloc0 (a->fsize  * sizeof(double));
	a->forfftout = (double *)malloc0 (a->msize  * sizeof(complex));
	a->cmask     = (double *)malloc0 (a->msize  * sizeof(double));
	a->mask      = (double *)malloc0 (a->msize  * sizeof(double));
	a->cfc_gain  = (double *)malloc0 (a->msize  * sizeof(double));
	a->revfftin  = (double *)malloc0 (a->msize  * sizeof(complex));
	a->revfftout = (double *)malloc0 (a->fsize  * sizeof(double));
	a->save      = (double **)malloc0(a->ovrlp  * sizeof(double *));
	for (i = 0; i < a->ovrlp; i++)
		a->save[i] = (double *)malloc0(a->fsize * sizeof(double));
	a->outaccum = (double *)malloc0(a->oasize * sizeof(double));
	a->nsamps = 0;
	a->saveidx = 0;
	a->Rfor = fftw_plan_dft_r2c_1d(a->fsize, a->forfftin, (fftw_complex *)a->forfftout, FFTW_ESTIMATE);
	a->Rrev = fftw_plan_dft_c2r_1d(a->fsize, (fftw_complex *)a->revfftin, a->revfftout, FFTW_ESTIMATE);
	calc_cfcwindow(a);

	a->pregain  = (2.0 * a->winfudge) / (double)a->fsize;
	a->postgain = 0.5 / ((double)a->ovrlp * a->winfudge);

	a->fpG = (double *) malloc0 ((a->max_freqs + 2) * sizeof (double));
	a->fpE = (double *) malloc0 ((a->max_freqs + 2) * sizeof(double));
	a->gp  = (double *) malloc0 ((a->max_freqs + 2) * sizeof (double));
	a->ep  = (double *) malloc0 ((a->max_freqs + 2) * sizeof (double));
	a->comp = (double *) malloc0 (a->msize * sizeof (double));
	a->peq  = (double *) malloc0 (a->msize * sizeof (double));
	calc_compG (a);
	calc_compE (a);

	a->gain = 0.0;
	a->mmult = exp (-1.0 / (a->rate * a->ovrlp * a->mtau));
	a->dmult = exp (-(double)a->fsize / (a->rate * a->ovrlp * a->dtau));

	a->delta         = (double*)malloc0 (a->msize * sizeof(double));
	a->delta_copy    = (double*)malloc0 (a->msize * sizeof(double));
	a->cfc_gain_copy = (double*)malloc0 (a->msize * sizeof(double));
}

void decalc_cfcomp(CFCOMP a)
{
	int i;
	_aligned_free (a->cfc_gain_copy);
	_aligned_free (a->delta_copy);
	_aligned_free (a->delta);
	_aligned_free (a->peq);
	_aligned_free (a->comp);
	_aligned_free (a->ep);
	_aligned_free (a->gp);
	_aligned_free (a->fpE);
	_aligned_free (a->fpG);

	fftw_destroy_plan(a->Rrev);
	fftw_destroy_plan(a->Rfor);
	_aligned_free(a->outaccum);
	for (i = 0; i < a->ovrlp; i++)
		_aligned_free(a->save[i]);
	_aligned_free(a->save);
	_aligned_free(a->revfftout);
	_aligned_free(a->revfftin);
	_aligned_free(a->cfc_gain);
	_aligned_free(a->mask);
	_aligned_free(a->cmask);
	_aligned_free(a->forfftout);
	_aligned_free(a->forfftin);
	_aligned_free(a->inaccum);
	_aligned_free(a->window);
}

CFCOMP create_cfcomp(int run, int position, int peq_run, int size, double* in, double* out, int fsize, int ovrlp,
	int rate, int wintype, int comp_method, int nfreqsG, int nfreqsE, double precomp, double prepeq,
	double* Fg, double* G, double* Fe, double* E, double mtau, double dtau)
{
	CFCOMP a = (CFCOMP) malloc0 (sizeof (cfcomp));
	
	a->run = run;
	a->position = position;
	a->peq_run = peq_run;
	a->bsize = size;
	a->in = in;
	a->out = out;
	a->fsize = fsize;
	a->ovrlp = ovrlp;
	a->rate = rate;
	a->wintype = wintype;
	a->comp_method = comp_method;
	a->max_freqs = 256;
	a->nfreqsG = nfreqsG;
	a->nfreqsE = nfreqsE;
	a->precomp = precomp;
	a->prepeq = prepeq;
	a->mtau = mtau;
	a->dtau = dtau;
	a->Fg = (double *)malloc0 (a->max_freqs * sizeof (double));
	a->Fe = (double *)malloc0 (a->max_freqs * sizeof (double));
	a->G  = (double *)malloc0 (a->max_freqs * sizeof (double));
	a->E  = (double *)malloc0 (a->max_freqs * sizeof (double));
	a->saryG = (double*)malloc0 (2 * a->max_freqs * sizeof (double));
	a->saryE = (double*)malloc0 (2 * a->max_freqs * sizeof (double));
	memcpy (a->Fg, Fg, a->nfreqsG * sizeof (double));
	memcpy (a->Fe, Fe, a->nfreqsE * sizeof(double));
	memcpy (a->G,  G,  a->nfreqsG * sizeof (double));
	memcpy (a->E,  E,  a->nfreqsE * sizeof (double));
	a->gdeg = 0;
	a->edeg = 0;
	a->png = create_nurbs (nfreqsG - 1, 3, 0, 0, 1024,
		EQ_MAXIMUM_CONTROL_POINTS,
		EQ_MAXIMUM_DEGREE,
		EQ_MAXIMUM_U_VALUES,
		EQ_MAXIMUM_FPTS);
	a->pne = create_nurbs (nfreqsE - 1, 3, 0, 0, 1024,
		EQ_MAXIMUM_CONTROL_POINTS,
		EQ_MAXIMUM_DEGREE,
		EQ_MAXIMUM_U_VALUES,
		EQ_MAXIMUM_FPTS);
	calc_cfcomp (a);
	return a;
}

void flush_cfcomp (CFCOMP a)
{
	int i;
	memset (a->inaccum, 0, a->iasize * sizeof (double));
	for (i = 0; i < a->ovrlp; i++)
		memset (a->save[i], 0, a->fsize * sizeof (double));
	memset (a->outaccum, 0, a->oasize * sizeof (double));
	a->nsamps   = 0;
	a->iainidx  = 0;
	a->iaoutidx = 0;
	a->oainidx  = a->init_oainidx;
	a->oaoutidx = 0;
	a->saveidx  = 0;
	a->gain = 0.0;
	memset(a->delta, 0, a->msize * sizeof(double));
}

void destroy_cfcomp (CFCOMP a)
{
	decalc_cfcomp (a);
	destroy_nurbs (a->pne);
	destroy_nurbs (a->png);
	_aligned_free (a->saryE);
	_aligned_free (a->saryG);
	_aligned_free (a->E);
	_aligned_free (a->G);
	_aligned_free (a->Fe);
	_aligned_free (a->Fg);
	_aligned_free (a);
}


void calc_mask (CFCOMP a)
{
	int i;
	double comp, mask, delta;
	switch (a->comp_method)
	{
	case 0:
		{
			double mag, test;
			for (i = 0; i < a->msize; i++)
			{
				mag = sqrt (a->forfftout[2 * i + 0] * a->forfftout[2 * i + 0] 
					      + a->forfftout[2 * i + 1] * a->forfftout[2 * i + 1]);
				comp = a->cfc_gain[i];
				test = comp * mag;
				if (test > 1.0)
					mask = 1.0 / mag;
				else
					mask = comp;
				a->cmask[i] = mask;
				if (test > a->gain) a->gain = test;
				else a->gain = a->mmult * a->gain;

				delta = a->cfc_gain[i] - a->cmask[i];
				if (delta > a->delta[i]) a->delta[i] = delta;
				else a->delta[i] *= a->dmult;
			}
			break;
		}
	}
	if (a->peq_run)
	{
		for (i = 0; i < a->msize; i++)
		{
			a->mask[i] = a->cmask[i] * a->prepeqlin * a->peq[i];
		}
	}
	else
		memcpy (a->mask, a->cmask, a->msize * sizeof (double));
	// print_impulse ("mask.txt", a->msize, a->mask, 0, 0);
	a->mask_ready = 1;
}

void xcfcomp (CFCOMP a, int pos)
{
	if (a->run && pos == a->position)
	{
		int i, j, k, sbuff, sbegin;
		for (i = 0; i < 2 * a->bsize; i += 2)
		{
			a->inaccum[a->iainidx] = a->in[i];
			a->iainidx = (a->iainidx + 1) % a->iasize;
		}
		a->nsamps += a->bsize;
		while (a->nsamps >= a->fsize)
		{
			for (i = 0, j = a->iaoutidx; i < a->fsize; i++, j = (j + 1) % a->iasize)
				a->forfftin[i] = a->pregain * a->window[i] * a->inaccum[j];
			a->iaoutidx = (a->iaoutidx + a->incr) % a->iasize;
			a->nsamps -= a->incr;
			fftw_execute (a->Rfor);
			calc_mask(a);
			for (i = 0; i < a->msize; i++)
			{
				a->revfftin[2 * i + 0] = a->mask[i] * a->forfftout[2 * i + 0];
				a->revfftin[2 * i + 1] = a->mask[i] * a->forfftout[2 * i + 1];
			}
			fftw_execute (a->Rrev);
			for (i = 0; i < a->fsize; i++)
				a->save[a->saveidx][i] = a->postgain * a->window[i] * a->revfftout[i];
			for (i = a->ovrlp; i > 0; i--)
			{
				sbuff = (a->saveidx + i) % a->ovrlp;
				sbegin = a->incr * (a->ovrlp - i);
				for (j = sbegin, k = a->oainidx; j < a->incr + sbegin; j++, k = (k + 1) % a->oasize)
				{
					if ( i == a->ovrlp)
						a->outaccum[k]  = a->save[sbuff][j];
					else
						a->outaccum[k] += a->save[sbuff][j];
				}
			}
			a->saveidx = (a->saveidx + 1) % a->ovrlp;
			a->oainidx = (a->oainidx + a->incr) % a->oasize;
		}
		for (i = 0; i < a->bsize; i++)
		{
			a->out[2 * i + 0] = a->outaccum[a->oaoutidx];
			a->out[2 * i + 1] = 0.0;
			a->oaoutidx = (a->oaoutidx + 1) % a->oasize;
		}
	}
	else if (a->out != a->in)
		memcpy (a->out, a->in, a->bsize * sizeof (complex));
}

void setBuffers_cfcomp (CFCOMP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_cfcomp (CFCOMP a, int rate)
{
	decalc_cfcomp (a);
	a->rate = rate;
	calc_cfcomp (a);
}

void setSize_cfcomp (CFCOMP a, int size)
{
	decalc_cfcomp (a);
	a->bsize = size;
	calc_cfcomp (a);
}

/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

// Use for both Compressor and Equalizer.  Per another call below, the Equalizer can be separately
//    turned OFF/ON, as long as this is set to Run.  However, the Equalizer cannot run without this
//    being set to Run and the Compressor also being enabled.  That should be OK as there is not much
//    reason to have this second equalizer unless the compressor is running.
PORT
void SetTXACFCOMPRun (int channel, int run)
{
	CFCOMP a = txa[channel].cfcomp.p;
	if (a->run != run)
	{
		EnterCriticalSection (&ch[channel].csDSP);
		a->run = run;
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

// Both the compressor and post-equalizer must go the same place in the TX audio processing pipeline; hence
//    there are NOT separate functions for them.
PORT 
void SetTXACFCOMPPosition (int channel, int pos)
{
	CFCOMP a = txa[channel].cfcomp.p;
	if (a->position != pos)
	{
		EnterCriticalSection (&ch[channel].csDSP);
		a->position = pos;
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

// This function was retained for interfaces, such as that of the Legacy UI,
// for which the Compressor and Equalizer have identical frequency sets.
PORT
void SetTXACFCOMPprofile (int channel, int nfreqs, double* F, double* G, double *E)
{
	CFCOMP a = txa[channel].cfcomp.p;
	EnterCriticalSection (&ch[channel].csDSP);
	a->nfreqsG = nfreqs;
	a->nfreqsE = nfreqs;
	memcpy (a->Fg, F, a->nfreqsG * sizeof (double));
	memcpy (a->Fe, F, a->nfreqsE * sizeof (double));
	memcpy (a->G,  G, a->nfreqsG * sizeof (double));
	memcpy (a->E,  E, a->nfreqsE * sizeof (double));
	calc_compG(a);
	calc_compE(a);
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Sets Frequencies and Compression for Compressor only.
PORT
void SetTXACFCOMPGprofile(int channel, int nfreqs, double* F, double* G)
{
	CFCOMP a = txa[channel].cfcomp.p;
	EnterCriticalSection(&ch[channel].csDSP);
	a->nfreqsG = nfreqs;
	memcpy(a->Fg, F, a->nfreqsG * sizeof(double));
	memcpy(a->G,  G, a->nfreqsG * sizeof(double));
	calc_compG(a);
	LeaveCriticalSection(&ch[channel].csDSP);
}

// Sets Frequencies and Gain for Equalizer only.
PORT
void SetTXACFCOMPEprofile(int channel, int nfreqs, double* F, double* E)
{
	CFCOMP a = txa[channel].cfcomp.p;
	EnterCriticalSection(&ch[channel].csDSP);
	a->nfreqsE = nfreqs;
	memcpy(a->Fe, F, a->nfreqsE * sizeof(double));
	memcpy(a->E,  E, a->nfreqsE * sizeof(double));
	calc_compE(a);
	LeaveCriticalSection(&ch[channel].csDSP);
}


// Flat compression gain (pre-compression).
PORT
void SetTXACFCOMPPrecomp (int channel, double precomp)
{
	CFCOMP a = txa[channel].cfcomp.p;
	if (a->precomp != precomp)
	{
		EnterCriticalSection (&ch[channel].csDSP);
		a->precomp = precomp;
		a->precomplin = pow (10.0, 0.05 * a->precomp);
		for (int i = 0; i < a->msize; i++)
		{
			a->cfc_gain[i] = a->precomplin * a->comp[i];
		}
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

// Turn Equalizer OFF/ON independently; but, Compressor must be enabled to use this Equalizer.
PORT
void SetTXACFCOMPPeqRun (int channel, int run)
{
	CFCOMP a = txa[channel].cfcomp.p;
	if (a->peq_run != run)
	{
		EnterCriticalSection (&ch[channel].csDSP);
		a->peq_run = run;
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

// Preamp that goes with the Equalizer
PORT
void SetTXACFCOMPPrePeq (int channel, double prepeq)
{
	CFCOMP a = txa[channel].cfcomp.p;
	EnterCriticalSection (&ch[channel].csDSP);
	a->prepeq = prepeq;
	a->prepeqlin = pow (10.0, 0.05 * a->prepeq);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void GetTXACFCOMPDisplayCompression(int channel, double* comp_values, int* ready)
{
	CFCOMP a = txa[channel].cfcomp.p;
	EnterCriticalSection(&ch[channel].csDSP);
	if (*ready = a->mask_ready)
	{
		memcpy(a->delta_copy, a->delta, a->msize * sizeof(double));
		memcpy(a->cfc_gain_copy, a->cfc_gain, a->msize * sizeof(double));
		a->mask_ready = 0;
	}
	LeaveCriticalSection(&ch[channel].csDSP);
	if (*ready)
	{
		int step = (a->msize - 1) / 1024;
		for (int i = 0, j = 0; i < 1025; i++, j += step)
		{
			double gain = a->cfc_gain_copy[j];
			double delta = a->delta_copy[j];
			if (delta <= 0.0 || gain <= 0.0)
			{
				comp_values[i] = 0.0;
			}
			else
			{
				double denom = gain - delta;
				if (denom <= 1e-20)
				{
					comp_values[i] = 40.0;
				}
				else
				{
					comp_values[i] = 20.0 * mlog10(gain / denom);
				}
			}
		}
	}
}

PORT
void SetTXACFCOMPCompCurve (int channel, int deg, int r, int umethod)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->png;
	EnterCriticalSection (&ch[channel].csDSP);
	a->gdeg = deg;
	b->p = deg;
	b->r = r;
	if (umethod == 1) b->umethod = umethod;
	else b->umethod = 0;
	if (!checkSplineInputs (a->nfreqsG, b->p, b->r, b->umethod, b->W))
	{
		calc_compG (a);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXACFCOMPCompWeights (int channel, int nfreq, double* weights)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->png;
	EnterCriticalSection (&ch[channel].csDSP);
	for (int i = 0; i < nfreq; i++)
		b->W[i] = weights[i];
	if (!checkSplineInputs (a->nfreqsG, b->p, b->r, b->umethod, b->W))
	{
		calc_compG (a);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void GetTXACFCOMPCompDraw (int channel, double* X, double* Y)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->png;
	EnterCriticalSection (&ch[channel].csDSP);
	memcpy(X, b->Xs, b->upts * sizeof(double));
	memcpy(Y, b->Ys, b->upts * sizeof(double));
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXACFCOMPPeqCurve (int channel, int deg, int r, int umethod)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->pne;
	EnterCriticalSection (&ch[channel].csDSP);
	a->edeg = deg;
	b->p = deg;
	b->r = r;
	if (umethod == 1) b->umethod = umethod;
	else b->umethod = 0;
	if (!checkSplineInputs (a->nfreqsE, b->p, b->r, b->umethod, b->W))
	{
		calc_compE (a);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXACFCOMPPeqWeights (int channel, int nfreq, double* weights)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->pne;
	EnterCriticalSection (&ch[channel].csDSP);
	for (int i = 0; i < nfreq; i++)
		b->W[i] = weights[i];
	if (!checkSplineInputs (a->nfreqsE, b->p, b->r, b->umethod, b->W))
	{
		calc_compE (a);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void GetTXACFCOMPPeqDraw (int channel, double* X, double* Y)
{
	CFCOMP a = txa[channel].cfcomp.p;
	NURBS b = a->pne;
	EnterCriticalSection (&ch[channel].csDSP);
	memcpy (X, b->Xs, b->upts * sizeof(double));
	memcpy (Y, b->Ys, b->upts * sizeof(double));
	LeaveCriticalSection (&ch[channel].csDSP);
}

