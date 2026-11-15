/*  nbp.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2015, 2016 Warren Pratt, NR0V

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
*											Notch Database												*
*																										*
********************************************************************************************************/

NOTCHDB create_notchdb (int master_run, int maxnotches)
{
	NOTCHDB a = (NOTCHDB) malloc0 (sizeof (notchdb));
	a->master_run = master_run;
	a->maxnotches = maxnotches;
	a->nn = 0;
	a->fcenter = (double *) malloc0 (a->maxnotches * sizeof (double));
	a->fwidth  = (double *) malloc0 (a->maxnotches * sizeof (double));
	a->nlow    = (double *) malloc0 (a->maxnotches * sizeof (double));
	a->nhigh   = (double *) malloc0 (a->maxnotches * sizeof (double));
	a->active  = (int    *) malloc0 (a->maxnotches * sizeof (int   ));
	return a;
}

void destroy_notchdb (NOTCHDB b)
{
	_aligned_free (b->active);
	_aligned_free (b->nhigh);
	_aligned_free (b->nlow);
	_aligned_free (b->fwidth);
	_aligned_free (b->fcenter);
	_aligned_free (b);
}

/********************************************************************************************************
*																										*
*										Notched Bandpass Filter											*
*																										*
********************************************************************************************************/

double* fir_mbandpass (int N, int nbp, double* flow, double* fhigh, double rate, double scale, int wintype)
{
	int i, k;
	double* impulse = (double *) malloc0 (N * sizeof (complex));
	double* imp;
	for (k = 0; k < nbp; k++)
	{
		imp = fir_bandpass (N, flow[k], fhigh[k], rate, wintype, 1, scale);
		for (i = 0; i < N; i++)
		{
			impulse[2 * i + 0] += imp[2 * i + 0];
			impulse[2 * i + 1] += imp[2 * i + 1];
		}
		_aligned_free (imp);
	}
	return impulse;
}

double min_notch_width (NBP a)
{
	double min_width;
	switch (a->wintype)
	{
	case 0:
		min_width = 1600.0 / (a->nc / 256) * (a->rate / 48000);
		break;
	case 1:
		min_width = 2200.0 / (a->nc / 256) * (a->rate / 48000);
		break;
	default:
		min_width = 2200.0 / (a->nc / 256) * (a->rate / 48000);
		break;
	}
	return min_width;
}

int make_nbp (int nn, int* active, double* center, double* width, double* nlow, double* nhigh, 
	double minwidth, int autoincr, double flow, double fhigh, double* bplow, double* bphigh, int* havnotch)
{
	int nbp;
	int nnbp, adds;
	int i, j, k;
	double nl, nh;
	int* del = (int *) malloc0 (1024 * sizeof (int));
	if (fhigh > flow)
	{
		bplow[0]  = flow;
		bphigh[0] = fhigh;
		nbp = 1;
	}
	else
	{
		nbp = 0;
		return nbp;
	}
	*havnotch = 0;
	for (k = 0; k < nn; k++)
	{
		if (autoincr && width[k] < minwidth)
		{
			nl = center[k] - 0.5 * minwidth;
			nh = center[k] + 0.5 * minwidth;
		}
		else
		{
			nl = nlow[k];
			nh = nhigh[k];
		}
		if (active[k] && (nh > flow && nl < fhigh))
		{
			*havnotch = 1;
			adds = 0;
			for (i = 0; i < nbp; i++)
			{
				if (nh > bplow[i] && nl < bphigh[i])
				{
					if (nl <= bplow[i] && nh >= bphigh[i])
					{
						del[i] = 1;
					}
					else if (nl > bplow[i] && nh < bphigh[i])
					{
						
						bplow[nbp + adds] = nh;
						bphigh[nbp + adds] = bphigh[i];
						bphigh[i] = nl;
						adds++;
					}
					else if (nl <= bplow[i] && nh > bplow[i])	
					{
						bplow[i] = nh;
					}
					else if (nl < bphigh[i] && nh >= bphigh[i])
					{
						bphigh[i] = nl;
					}
				}
			}
			nbp += adds;
			nnbp = nbp;
			for (i = 0; i < nbp; i++)
			{
				if (del[i] == 1)
				{
					nnbp--;
					for (j = i; j < nnbp; j++)
					{
						bplow[j] = bplow[j + 1];
						bphigh[j] = bphigh[j + 1];
					}
					del[i] = 0;
				}
			}
			nbp = nnbp;
		}
	}
	_aligned_free (del);
	return nbp;
}

void calc_nbp_lightweight (NBP a)
{	// calculate and set new impulse response; used when changing tune freq or shift freq
	int i;
	double fl, fh;
	double offset;
	NOTCHDB b = *a->ptraddr;
	if (a->fnfrun)
	{
		offset = b->tunefreq + b->shift;
		fl = a->flow  + offset;
		fh = a->fhigh + offset;
		a->numpb = make_nbp (b->nn, b->active, b->fcenter, b->fwidth, b->nlow, b->nhigh, 
			min_notch_width (a), a->autoincr, fl, fh, a->bplow, a->bphigh, &a->havnotch);
		// when tuning, no need to recalc filter if there were not and are not any notches in passband
		if (a->hadnotch || a->havnotch)
		{
			for (i = 0; i < a->numpb; i++)
			{
				a->bplow[i]  -=	offset;
				a->bphigh[i] -= offset;
			}
			a->impulse = fir_mbandpass (a->nc, a->numpb, a->bplow, a->bphigh,
				a->rate, a->gain / (double)(2 * a->size), a->wintype);
			setImpulse_fircore (a->p, a->impulse, 1);
			// print_impulse ("nbp.txt", a->size + 1, impulse, 1, 0);
			_aligned_free(a->impulse);
		}
		a->hadnotch = a->havnotch;
	}
	else
		a->hadnotch = 1;
}

void calc_nbp_impulse (NBP a)
{	// calculates impulse response; for create_fircore() and parameter changes
	int i;
	double fl, fh;
	double offset;
	NOTCHDB b = *a->ptraddr;
	if (a->fnfrun)
	{
		offset = b->tunefreq + b->shift;
		fl = a->flow  + offset;
		fh = a->fhigh + offset;
		a->numpb = make_nbp (b->nn, b->active, b->fcenter, b->fwidth, b->nlow, b->nhigh, 
			min_notch_width (a), a->autoincr, fl, fh, a->bplow, a->bphigh, &a->havnotch);
		for (i = 0; i < a->numpb; i++)
		{
			a->bplow[i]  -=	offset;
			a->bphigh[i] -= offset;
		}
		a->impulse = fir_mbandpass (a->nc, a->numpb, a->bplow, a->bphigh,
			a->rate, a->gain / (double)(2 * a->size), a->wintype);
	}
	else
	{
		a->impulse = fir_bandpass(a->nc, a->flow, a->fhigh, a->rate, a->wintype, 1, a->gain / (double)(2 * a->size));
	}
}

NBP create_nbp(int run, int fnfrun, int position, int size, int nc, int mp, double* in, double* out, 
	double flow, double fhigh, int rate, int wintype, double gain, int autoincr, int maxpb, NOTCHDB* ptraddr)
{
	NBP a = (NBP) malloc0 (sizeof (nbp));
	a->run = run;
	a->fnfrun = fnfrun;
	a->position = position;
	a->size = size;
	a->nc = nc;
	a->mp = mp;
	a->rate = (double)rate;
	a->wintype = wintype;
	a->gain = gain;
	a->in = in;
	a->out = out;
	a->autoincr = autoincr;
	a->flow = flow;
	a->fhigh = fhigh;
	a->maxpb = maxpb;
	a->ptraddr = ptraddr;
	a->bplow   = (double *) malloc0 (a->maxpb * sizeof (double));
	a->bphigh  = (double *) malloc0 (a->maxpb * sizeof (double));
	calc_nbp_impulse (a);
	a->p = create_fircore (a->size, a->in, a->out, a->nc, a->mp, 16, a->impulse);
	// print_impulse ("nbp.txt", a->size + 1, impulse, 1, 0);
	_aligned_free(a->impulse);
	return a;
}

void destroy_nbp (NBP a)
{
	destroy_fircore (a->p);
	_aligned_free (a->bphigh);
	_aligned_free (a->bplow);
	_aligned_free (a);
}

void flush_nbp (NBP a)
{
	flush_fircore (a->p);
}

void xnbp (NBP a, int pos)
{
	if (a->run && pos == a->position)
		xfircore (a->p);
	else if (a->in != a->out)
		memcpy (a->out, a->in, a->size * sizeof (complex));
}

void setBuffers_nbp (NBP a, double* in, double* out)
{
	a->in = in;
	a->out = out;
	setBuffers_fircore (a->p, a->in, a->out);
}

void setSamplerate_nbp (NBP a, int rate)
{
	a->rate = rate;
	calc_nbp_impulse (a);
	setImpulse_fircore (a->p, a->impulse, 1);
	_aligned_free (a->impulse);
}

void setSize_nbp (NBP a, int size)
{
	// NOTE:  'size' must be <= 'nc'
	a->size = size;
	setSize_fircore (a->p, a->size);
	calc_nbp_impulse (a);
	setImpulse_fircore (a->p, a->impulse, 1);
	_aligned_free (a->impulse);
}

void setNc_nbp (NBP a)
{
	calc_nbp_impulse (a);
	setNc_fircore (a->p, a->nc, a->impulse);
	_aligned_free (a->impulse);
}

void setMp_nbp (NBP a)
{
	setMp_fircore (a->p, a->mp);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

// DATABASE PROPERTIES

void UpdateNBPFiltersLightWeight (int channel)
{	// called when setting tune freq or shift freq
	calc_nbp_lightweight (rxa[channel].nbp0.p);
	calc_nbp_lightweight (rxa[channel].bpsnba.p->bpsnba);
}

void UpdateNBPFilters(int channel)
{
	NBP a = rxa[channel].nbp0.p;
	BPSNBA b = rxa[channel].bpsnba.p;
	if (a->fnfrun)
	{
		calc_nbp_impulse (a);
		setImpulse_fircore (a->p, a->impulse, 1);
		_aligned_free (a->impulse);
	}
	if (b->bpsnba->fnfrun)
	{
		recalc_bpsnba_filter (b, 1);
	}
}

PORT
int RXANBPAddNotch (int channel, int notch, double fcenter, double fwidth, int active)
{
	NOTCHDB b;
	int i, j;
	int rval;
	b = rxa[channel].ndb.p;
	if (notch <= b->nn && b->nn < b->maxnotches)
	{
		b->nn++;
		for (i = b->nn - 2, j = b->nn - 1; i >= notch; i--, j--)
		{
			b->fcenter[j] = b->fcenter[i];
			b->fwidth[j] = b->fwidth[i];
			b->nlow[j] = b->nlow[i];
			b->nhigh[j] = b->nhigh[i];
			b->active[j] = b->active[i];
		}
		b->fcenter[notch] = fcenter;
		b->fwidth[notch] = fwidth;
		b->nlow[notch] = fcenter - 0.5 * fwidth;
		b->nhigh[notch] = fcenter + 0.5 * fwidth;
		b->active[notch] = active;
		UpdateNBPFilters (channel);
		rval = 0;
	}
	else
		rval = -1;
	return rval;
}

PORT
int RXANBPGetNotch (int channel, int notch, double* fcenter, double* fwidth, int* active)
{
	NOTCHDB a;
	int rval;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].ndb.p;
	if (notch < a->nn)
	{
		*fcenter = a->fcenter[notch];
		*fwidth = a->fwidth[notch];
		*active = a->active[notch];
		rval = 0;
	}
	else
	{
		*fcenter = -1.0;
		*fwidth = 0.0;
		*active = -1;
		rval = -1;
	}
	LeaveCriticalSection (&ch[channel].csDSP);
	return rval;
}

PORT
int RXANBPDeleteNotch (int channel, int notch)
{
	int i, j;
	int rval;
	NOTCHDB a;
	a = rxa[channel].ndb.p;
	if (notch < a->nn)
	{
		a->nn--;
		for (i = notch, j = notch + 1; i < a->nn; i++, j++)
		{
			a->fcenter[i] = a->fcenter[j];
			a->fwidth[i] = a->fwidth[j];
			a->nlow[i] = a->nlow[j];
			a->nhigh[i] = a->nhigh[j];
			a->active[i] = a->active[j];
		}
		UpdateNBPFilters (channel);
		rval = 0;
	}
	else
		rval = -1;
	return rval;
}

PORT
int RXANBPEditNotch (int channel, int notch, double fcenter, double fwidth, int active)
{
	NOTCHDB a;
	int rval;
	a = rxa[channel].ndb.p;
	if (notch < a->nn)
	{
		a->fcenter[notch] = fcenter;
		a->fwidth[notch] = fwidth;
		a->nlow[notch] = fcenter - 0.5 * fwidth;
		a->nhigh[notch] = fcenter + 0.5 * fwidth;
		a->active[notch] = active;
		UpdateNBPFilters (channel);
		rval = 0;
	}
	else
		rval = -1;
	return rval;
}

PORT
void RXANBPGetNumNotches (int channel, int* nnotches)
{
	NOTCHDB a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].ndb.p;
	*nnotches = a->nn;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void RXANBPSetTuneFrequency (int channel, double tunefreq)
{
	NOTCHDB a;
	a = rxa[channel].ndb.p;
	if (tunefreq != a->tunefreq)
	{
		a->tunefreq = tunefreq;
		UpdateNBPFiltersLightWeight (channel);
	}
}

PORT
void RXANBPSetShiftFrequency (int channel, double shift)
{
	NOTCHDB a;
	a = rxa[channel].ndb.p;
	if (shift != a->shift)
	{
		a->shift = shift;
		UpdateNBPFiltersLightWeight (channel);
	}
}

PORT
void RXANBPSetNotchesRun (int channel, int run)
{
	NOTCHDB a = rxa[channel].ndb.p; 
	NBP b = rxa[channel].nbp0.p;
	if ( run != a->master_run)
	{
		a->master_run = run;							// update variables
		b->fnfrun = a->master_run;
		RXAbpsnbaCheck (channel, rxa[channel].mode, run);
		calc_nbp_impulse (b);							// recalc nbp impulse response
		setImpulse_fircore (b->p, b->impulse, 0);		// calculate new filter masks
		_aligned_free (b->impulse);
		EnterCriticalSection (&ch[channel].csDSP);		// block DSP channel processing
		RXAbpsnbaSet (channel);
		setUpdate_fircore (b->p);						// apply new filter masks
		LeaveCriticalSection (&ch[channel].csDSP);		// unblock channel processing
	}
}

// FILTER PROPERTIES

PORT
void RXANBPSetRun (int channel, int run)
{
	NBP a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].nbp0.p;
	a->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void RXANBPSetFreqs (int channel, double flow, double fhigh)
{
	NBP a;
	a = rxa[channel].nbp0.p;
	if ((flow != a->flow) || (fhigh != a->fhigh))
	{
		a->flow = flow;
		a->fhigh = fhigh;
		calc_nbp_impulse (a);
		setImpulse_fircore (a->p, a->impulse, 1);
		_aligned_free (a->impulse);
	}
}

PORT
void RXANBPSetWindow (int channel, int wintype)
{
	NBP a;
	BPSNBA b;
	a = rxa[channel].nbp0.p;
	b = rxa[channel].bpsnba.p;
	if ((a->wintype != wintype))
	{
		a->wintype = wintype;
		calc_nbp_impulse (a);
		setImpulse_fircore (a->p, a->impulse, 1);
		_aligned_free (a->impulse);
	}
	if ((b->wintype != wintype))
	{
		b->wintype = wintype;
		recalc_bpsnba_filter (b, 1);
	}
}

PORT
void RXANBPSetNC (int channel, int nc)
{
	// NOTE:  'nc' must be >= 'size'
	NBP a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].nbp0.p;
	if (a->nc != nc)
	{
		a->nc = nc;
		setNc_nbp (a);
	}
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void RXANBPSetMP (int channel, int mp)
{
	NBP a;
	a = rxa[channel].nbp0.p;
	if (a->mp != mp)
	{
		a->mp = mp;
		setMp_nbp (a);
	}
}

PORT
void RXANBPGetMinNotchWidth (int channel, double* minwidth)
{
	NBP a;
	EnterCriticalSection (&ch[channel].csDSP);
	a = rxa[channel].nbp0.p;
	*minwidth = min_notch_width (a);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void RXANBPSetAutoIncrease (int channel, int autoincr)
{
	NBP a;
	BPSNBA b;
	a = rxa[channel].nbp0.p;
	b = rxa[channel].bpsnba.p;
	if ((a->autoincr != autoincr))
	{
		a->autoincr = autoincr;
		calc_nbp_impulse (a);
		setImpulse_fircore (a->p, a->impulse, 1);
		_aligned_free (a->impulse);
	}
	if ((b->autoincr != autoincr))
	{
		b->autoincr = autoincr;
		recalc_bpsnba_filter (b, 1);
	}
}
