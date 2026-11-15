/*  nnr.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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
#include "nnet.h"

#define NNR_PI			3.14159265358979323846
#define NNR_TWOPI		6.28318530717958647692
#define NNR_KAISER_BETA	7.857
#define NNR_PHASELEN	51

#define NNR_NRATE_DEFAULT		16000
#define NNR_FFTSIZE_DEFAULT		512
#define NNR_HOP_DEFAULT			256

typedef struct _nnrstft
{
	int valid;
	int fftsize;
	int overlap;
	int hop;
	int nbins;
	int lookahead;
	double olascale;
	double* inacc;
	double* abuf;
	double* oacc;
	double* awin;
	double* swin;
	double* tdin;
	double* tdout;
	double* fdout;
	double* fdin;
	double* xre;
	double* xim;
	double* yre;
	double* yim;
	fftw_plan pfor;
	fftw_plan prev;
	int prime;
	int delay;
} nnrstft;

typedef struct _nnr
{
	int run;
	int position;
	int size;
	double* in_buff;
	double* out_buff;
	int rate;
	int nrate;
	int fftsize;
	int overlap;
	int hop;
	int nbins;
	int lookahead;
	double mask_floor;
	int cmode;

	int decim;
	int nphase;
	int nproto;
	double* proto;
	double* dline_dec;
	int dpos_dec;
	int dcount;
	double* dline_int;
	int dpos_int;

	double* inacc;
	int nacc;
	double* abuf;
	double* oacc;
	double* awin;
	double* swin;
	double olascale;

	double* tdin;
	double* tdout;
	double* fdout;
	double* fdin;
	fftw_plan pfor;
	fftw_plan prev;

	double* xre;
	double* xim;
	double* yre;
	double* yim;
	NNET net;
	NNET nets[NNET_NSLOTS];
	nnrstft st[NNET_NSLOTS];
	int  model;

	double* orbuf;
	int orsize;
	int orread;
	int orwrite;
	int orcount;
	int prime;
	int delay;

	int nunder;
	int nover;
} nnr, * NNR;

static double nnr_i0 (double x)
{
	double sum = 1.0, t = 1.0, xh = 0.5 * x, t2;
	int k;
	for (k = 1; k <= 60; k++)
	{
		t *= xh / (double)k;
		t2 = t * t;
		sum += t2;
		if (t2 < 1.0e-22 * sum) break;
	}
	return sum;
}

static void nnr_design_proto (NNR a)
{
	int n, M = a->nproto;
	double c = 0.5 * (double)(M - 1);
	double ic = 1.0 / nnr_i0 (NNR_KAISER_BETA);
	double fc = 0.45 * (double)a->nrate / (double)a->rate;
	double x, u, s, w, sum = 0.0;

	for (n = 0; n < M; n++)
	{
		x = (double)n - c;
		if (fabs (x) < 1.0e-12) s = 2.0 * fc;
		else                    s = sin (NNR_TWOPI * fc * x) / (NNR_PI * x);
		u = 2.0 * (double)n / (double)(M - 1) - 1.0;
		u = 1.0 - u * u;
		if (u < 0.0) u = 0.0;
		w = nnr_i0 (NNR_KAISER_BETA * sqrt (u)) * ic;
		a->proto[n] = s * w;
		sum += a->proto[n];
	}
	for (n = 0; n < M; n++) a->proto[n] /= sum;
}

static void nnr_design_windows (nnrstft* t)
{
	int n;
	double h;
	for (n = 0; n < t->fftsize; n++)
	{
		h = 0.5 * (1.0 - cos (NNR_TWOPI * (double)n / (double)t->fftsize));
		t->awin[n] = sqrt (h);
		t->swin[n] = sqrt (h);
	}
	t->olascale = (2.0 / (double)t->overlap) / (double)t->fftsize;
}


static int nnr_build_stft (NNR a, int slot, int fftsize, int hop,
	int lookahead)
{
	nnrstft* t = &a->st[slot];

	if (fftsize < 16 || hop < 1 || fftsize % hop != 0) return 0;

	t->fftsize   = fftsize;
	t->hop       = hop;
	t->overlap   = fftsize / hop;
	t->nbins     = fftsize / 2 + 1;
	t->lookahead = lookahead;

	t->inacc = (double*) malloc0 (hop            * sizeof (double));
	t->abuf  = (double*) malloc0 (fftsize        * sizeof (double));
	t->oacc  = (double*) malloc0 (fftsize        * sizeof (double));
	t->awin  = (double*) malloc0 (fftsize        * sizeof (double));
	t->swin  = (double*) malloc0 (fftsize        * sizeof (double));
	t->tdin  = (double*) malloc0 (fftsize        * sizeof (double));
	t->tdout = (double*) malloc0 (fftsize        * sizeof (double));
	t->fdout = (double*) malloc0 (2 * t->nbins   * sizeof (double));
	t->fdin  = (double*) malloc0 (2 * t->nbins   * sizeof (double));
	t->xre   = (double*) malloc0 (t->nbins       * sizeof (double));
	t->xim   = (double*) malloc0 (t->nbins       * sizeof (double));
	t->yre   = (double*) malloc0 (t->nbins       * sizeof (double));
	t->yim   = (double*) malloc0 (t->nbins       * sizeof (double));

	nnr_design_windows (t);

	t->pfor = fftw_plan_dft_r2c_1d (fftsize, t->tdin,
		(fftw_complex*)t->fdout, FFTW_PATIENT);
	t->prev = fftw_plan_dft_c2r_1d (fftsize, (fftw_complex*)t->fdin,
		t->tdout, FFTW_PATIENT);

	t->prime = a->decim * hop;
	t->delay = a->decim * (fftsize - hop + lookahead * hop)
			 + a->nproto - 1
			 + t->prime;

	t->valid = 1;
	return 1;
}


static void nnr_free_stft (NNR a, int slot)
{
	nnrstft* t = &a->st[slot];
	if (!t->valid) return;
	fftw_destroy_plan (t->prev);
	fftw_destroy_plan (t->pfor);
	_aligned_free (t->yim);   _aligned_free (t->yre);
	_aligned_free (t->xim);   _aligned_free (t->xre);
	_aligned_free (t->fdin);  _aligned_free (t->fdout);
	_aligned_free (t->tdout); _aligned_free (t->tdin);
	_aligned_free (t->swin);  _aligned_free (t->awin);
	_aligned_free (t->oacc);  _aligned_free (t->abuf);
	_aligned_free (t->inacc);
	memset (t, 0, sizeof (nnrstft));
}


static void nnr_activate (NNR a, int slot)
{
	nnrstft* t = &a->st[slot];
	a->fftsize   = t->fftsize;
	a->overlap   = t->overlap;
	a->hop       = t->hop;
	a->nbins     = t->nbins;
	a->lookahead = t->lookahead;
	a->olascale  = t->olascale;
	a->inacc = t->inacc;  a->abuf  = t->abuf;   a->oacc  = t->oacc;
	a->awin  = t->awin;   a->swin  = t->swin;
	a->tdin  = t->tdin;   a->tdout = t->tdout;
	a->fdout = t->fdout;  a->fdin  = t->fdin;
	a->xre   = t->xre;    a->xim   = t->xim;
	a->yre   = t->yre;    a->yim   = t->yim;
	a->pfor  = t->pfor;   a->prev  = t->prev;
	a->prime = t->prime;  a->delay = t->delay;
}



static void nnr_fifo_put (NNR a, double v)
{
	if (a->orcount >= a->orsize)
	{
		a->orread = (a->orread + 1) % a->orsize;
		a->orcount--;
		a->nover++;
	}
	a->orbuf[a->orwrite] = v;
	a->orwrite = (a->orwrite + 1) % a->orsize;
	a->orcount++;
}

static double nnr_fifo_get (NNR a)
{
	double v;
	if (a->orcount <= 0)
	{
		a->nunder++;
		return 0.0;
	}
	v = a->orbuf[a->orread];
	a->orread = (a->orread + 1) % a->orsize;
	a->orcount--;
	return v;
}

static void nnr_push_output (NNR a, double y)
{
	int k, p, q;
	double acc;

	q = a->dpos_int;
	a->dline_int[q] = y;
	a->dline_int[q + a->nphase] = y;
	if (++a->dpos_int >= a->nphase) a->dpos_int = 0;

	for (p = 0; p < a->decim; p++)
	{
		acc = 0.0;
		for (k = 0; k < a->nphase; k++)
			acc += a->proto[k * a->decim + p] * a->dline_int[q + a->nphase - k];
		nnr_fifo_put (a, acc * (double)a->decim);
	}
}

static void nnr_frame (NNR a)
{
	int k;

	for (k = 0; k < a->fftsize; k++)
		a->tdin[k] = a->abuf[k] * a->awin[k];

	fftw_execute (a->pfor);

	for (k = 0; k < a->nbins; k++)
	{
		a->xre[k] = a->fdout[2 * k + 0];
		a->xim[k] = a->fdout[2 * k + 1];
	}

	run_nnet (a->net, a->xre, a->xim, a->yre, a->yim);

	for (k = 0; k < a->nbins; k++)
	{
		a->fdin[2 * k + 0] = a->yre[k];
		a->fdin[2 * k + 1] = a->yim[k];
	}
	a->fdin[1] = 0.0;
	a->fdin[2 * (a->nbins - 1) + 1] = 0.0;

	fftw_execute (a->prev);

	for (k = 0; k < a->fftsize; k++)
		a->oacc[k] += a->tdout[k] * a->swin[k] * a->olascale;

	for (k = 0; k < a->hop; k++)
		nnr_push_output (a, a->oacc[k]);

	memmove (a->oacc, a->oacc + a->hop, (a->fftsize - a->hop) * sizeof (double));
	memset  (a->oacc + a->fftsize - a->hop, 0, a->hop * sizeof (double));
}

static void nnr_push_input (NNR a, double x)
{
	int j, p;
	double acc;

	p = a->dpos_dec;
	a->dline_dec[p] = x;
	a->dline_dec[p + a->nproto] = x;
	if (++a->dpos_dec >= a->nproto) a->dpos_dec = 0;

	if (++a->dcount < a->decim) return;
	a->dcount = 0;

	acc = 0.0;
	for (j = 0; j < a->nproto; j++)
		acc += a->proto[j] * a->dline_dec[p + a->nproto - j];

	a->inacc[a->nacc++] = acc;
	if (a->nacc >= a->hop)
	{
		a->nacc = 0;
		memmove (a->abuf, a->abuf + a->hop, (a->fftsize - a->hop) * sizeof (double));
		memcpy  (a->abuf + a->fftsize - a->hop, a->inacc, a->hop * sizeof (double));
		nnr_frame (a);
	}
}

static void nnr_reset_state (NNR a)
{
	int i;

	memset (a->dline_dec, 0, 2 * a->nproto  * sizeof (double));
	memset (a->dline_int, 0, 2 * a->nphase  * sizeof (double));
	memset (a->inacc,     0,     a->hop     * sizeof (double));
	memset (a->abuf,      0,     a->fftsize * sizeof (double));
	memset (a->oacc,      0,     a->fftsize * sizeof (double));
	memset (a->orbuf,     0,     a->orsize  * sizeof (double));

	a->dpos_dec = 0;
	a->dcount   = 0;
	a->dpos_int = 0;
	a->nacc     = 0;
	a->orread   = 0;
	a->orwrite  = 0;
	a->orcount  = 0;
	a->nunder   = 0;
	a->nover    = 0;

	flush_nnet (a->net);

	for (i = 0; i < a->prime; i++) nnr_fifo_put (a, 0.0);
}

static void calc_nnr (NNR a)
{
	{
		int snb, snr, snf, shp, sla, k;
		double band_lo, band_hi;

		for (k = 0; k < NNET_NSLOTS; k++)
			if (nnet_probe_slot (k, &snb, &snr, &snf, &shp, &sla, &band_lo, &band_hi))
			{
				if (snr != a->nrate)
					dprintf ("nnr: slot %d sets the network rate to %d Hz "
							 "(was %d)\n", k, snr, a->nrate);
				a->nrate = snr;
				break;
			}
	}

	a->decim  = a->rate / a->nrate;

	if (a->decim < 1 || a->decim * a->nrate != a->rate)
	{
		dprintf ("nnr: dsp_rate %d is not an integer multiple of nrate %d - block disabled\n",
			a->rate, a->nrate);
		a->decim = 1;
		a->run = 0;
	}

	a->nphase = NNR_PHASELEN;
	a->nproto = a->nphase * a->decim;

	a->proto     = (double*) malloc0 (a->nproto      * sizeof (double));
	a->dline_dec = (double*) malloc0 (2 * a->nproto  * sizeof (double));
	a->dline_int = (double*) malloc0 (2 * a->nphase  * sizeof (double));
	nnr_design_proto (a);

	{
		int k, first = -1, maxor = 0;

		for (k = 0; k < NNET_NSLOTS; k++)
		{
			a->nets[k] = 0;
			memset (&a->st[k], 0, sizeof (nnrstft));
		}

		for (k = 0; k < NNET_NSLOTS; k++)
		{
			NNET m;
			int snb, snr, snf, shp, sla, or;
			double band_low, band_high;

			if (!nnet_probe_slot (k, &snb, &snr, &snf, &shp, &sla, &band_low, &band_high))
				continue;

			if (snr != a->nrate)
			{
				dprintf ("nnr: model slot %d rejected - it wants %d Hz and "
						 "the channel resamples to %d\n", k, snr, a->nrate);
				continue;
			}

			if (!nnr_build_stft (a, k, snf, shp, sla))
			{
				dprintf ("nnr: model slot %d rejected - N %d with hop %d is "
						 "not a usable transform\n", k, snf, shp);
				continue;
			}

			m = create_nnet_slot (k, a->st[k].nbins, a->st[k].lookahead,
								  a->mask_floor,
								  (double)a->nrate / (double)a->st[k].hop,
		                          band_low, band_high);
			if (m == 0 || !ok_nnet (m)
				|| getBins_nnet (m) != a->st[k].nbins)
			{
				if (m)
				{
					dprintf ("nnr: model slot %d rejected - it wants %d "
							 "bins, its transform gives %d\n",
							 k, getBins_nnet (m), a->st[k].nbins);
					destroy_nnet (m);
				}
				nnr_free_stft (a, k);
				continue;
			}

			a->nets[k] = m;
			if (first < 0) first = k;

			or = a->st[k].prime + 2 * a->size
			   + a->decim * (a->st[k].fftsize + a->st[k].hop) + 64;
			if (or > maxor) maxor = or;

			dprintf ("nnr: model slot %d ready - N %d, hop %d (overlap %d), "
					 "%.1f frames/s, %d frame(s) of lookahead, delay %d "
					 "(%.2f ms)\n", k, a->st[k].fftsize, a->st[k].hop,
					 a->st[k].overlap,
					 (double)a->nrate / (double)a->st[k].hop,
					 a->st[k].lookahead, a->st[k].delay,
					 1000.0 * (double)a->st[k].delay / (double)a->rate);
		}

		if (first < 0)
		{
			dprintf ("nnr: no usable model in any slot - passing audio "
					 "through\n");
			nnr_build_stft (a, 0, NNR_FFTSIZE_DEFAULT, NNR_HOP_DEFAULT, 0);
			first = 0;
			maxor = a->st[0].prime + 2 * a->size
				  + a->decim * (a->st[0].fftsize + a->st[0].hop) + 64;
		}

		a->orsize = maxor;
		a->orbuf  = (double*) malloc0 (a->orsize * sizeof (double));

		a->model = first;
		a->net   = a->nets[first];
		nnr_activate (a, first);
	}

	nnr_reset_state (a);

	dprintf ("nnr: rate=%d nrate=%d decim=%d N=%d hop=%d bins=%d proto=%d "
		"prime=%d delay=%d (%.2f ms)\n",
		a->rate, a->nrate, a->decim, a->fftsize, a->hop, a->nbins, a->nproto,
		a->prime, a->delay, 1000.0 * (double)a->delay / (double)a->rate);
}

static void decalc_nnr (NNR a)
{
	int k;

	for (k = 0; k < NNET_NSLOTS; k++)
		if (a->nets[k]) { destroy_nnet (a->nets[k]); a->nets[k] = 0; }
	a->net = 0;

	for (k = 0; k < NNET_NSLOTS; k++) nnr_free_stft (a, k);

	a->inacc = a->abuf = a->oacc = a->awin = a->swin = 0;
	a->tdin = a->tdout = a->fdout = a->fdin = 0;
	a->xre = a->xim = a->yre = a->yim = 0;

	_aligned_free (a->orbuf);
	_aligned_free (a->dline_int);
	_aligned_free (a->dline_dec);
	_aligned_free (a->proto);
}

NNR create_nnr
	(
	int run,
	int position,
	int size,
	double* in_buff,
	double* out_buff,
	int rate,
	double mask_floor,
	int cmode
	)
{
	NNR a = (NNR) malloc0 (sizeof (nnr));

	a->run        = run;
	a->position   = position;
	a->size       = size;
	a->in_buff    = in_buff;
	a->out_buff   = out_buff;
	a->rate       = rate;
	a->mask_floor = mask_floor;
	a->cmode      = cmode;

	a->nrate      = NNR_NRATE_DEFAULT;
	a->fftsize    = NNR_FFTSIZE_DEFAULT;
	a->overlap    = NNR_FFTSIZE_DEFAULT / NNR_HOP_DEFAULT;
	a->lookahead  = 0;

	calc_nnr (a);
	return a;
}

void destroy_nnr (NNR a)
{
	decalc_nnr (a);
	_aligned_free (a);
}

void flush_nnr (NNR a)
{
	nnr_reset_state (a);
}

void xnnr (NNR a, int pos)
{
	if (a->run && a->position == pos)
	{
		int i;
		double y;

		for (i = 0; i < a->size; i++)
			nnr_push_input (a, a->in_buff[2 * i + 0]);

		for (i = 0; i < a->size; i++)
		{
			y = nnr_fifo_get (a);
			a->out_buff[2 * i + 0] = y;
			a->out_buff[2 * i + 1] = (a->cmode == 0) ? y : 0.0;
		}

		if (a->nunder || a->nover)
		{
			dprintf ("nnr: FIFO under=%d over=%d count=%d size=%d\n",
				a->nunder, a->nover, a->orcount, a->orsize);
			a->nunder = 0;
			a->nover  = 0;
		}
	}
	else if (a->in_buff != a->out_buff)
		memcpy (a->out_buff, a->in_buff, a->size * sizeof (complex));
}

void setBuffers_nnr (NNR a, double* in, double* out)
{
	a->in_buff  = in;
	a->out_buff = out;
}

void setSamplerate_nnr (NNR a, int rate)
{
	int keep = a->model;
	decalc_nnr (a);
	a->rate = rate;
	calc_nnr (a);
	setModel_nnr (a, keep);
}

void setSize_nnr (NNR a, int size)
{
	int keep = a->model;
	decalc_nnr (a);
	a->size = size;
	calc_nnr (a);
	setModel_nnr (a, keep);
}

#define NNR_ALL_MODELS(a, call)                                            \
	do {                                                                   \
		int _k;                                                            \
		for (_k = 0; _k < NNET_NSLOTS; _k++)                               \
			if ((a)->nets[_k]) { NNET n = (a)->nets[_k]; call; }           \
	} while (0)

int setModel_nnr (NNR a, int slot)
{
	if (slot < 0 || slot >= NNET_NSLOTS || a->nets[slot] == 0
		|| !a->st[slot].valid)
	{
		dprintf ("nnr: model slot %d is not loaded - staying on slot %d\n",
				 slot, a->model);
		return a->model;
	}
	if (slot == a->model) return a->model;

	a->model = slot;
	a->net   = a->nets[slot];

	nnr_activate (a, slot);
	nnr_reset_state (a);

	dprintf ("nnr: switched to model slot %d - N %d, hop %d (overlap %d), "
			 "%.1f frames/s, lookahead %d, delay %d (%.2f ms)\n",
			 slot, a->fftsize, a->hop, a->overlap,
			 (double)a->nrate / (double)a->hop, a->lookahead, a->delay,
			 1000.0 * (double)a->delay / (double)a->rate);
	return a->model;
}

int getModel_nnr (NNR a)
{
	return a->model;
}

int getDelay_nnr (NNR a)
{
	return a->delay;
}

int getRun_nnr (NNR a)
{
	return a->run;
}

/*  Standalone control surface.

	Every control below has an RXA property wrapper further down this file that
	does the same work under ch[channel].csDSP. These take an NNR directly, so a
	host that runs the block outside a channel -- as the impulse noise blanker is
	already run, via create_anbEXT()/xanbEXT() -- can reach the same settings.
	Locking is the caller's, because a standalone block has no channel whose
	critical section to take.  */

void setRun_nnr (NNR a, int run)
{
	if (a->run != run)
	{
		a->run = run;
		flush_nnr (a);
	}
}

void setPosition_nnr (NNR a, int position)
{
	a->position = position;
	flush_nnr (a);
}

void setCmode_nnr (NNR a, int cmode)
{
	a->cmode = cmode;
}

void setMaskFloor_nnr (NNR a, double floor_db)
{
	a->mask_floor = floor_db;
	NNR_ALL_MODELS (a, setFloor_nnet (n, floor_db));
}

void setTestMode_nnr (NNR a, int mode)
{
	NNR_ALL_MODELS (a, setMode_nnet (n, mode));
	flush_nnr (a);
}

void setAlpha_nnr (NNR a, double alpha)
{
	NNR_ALL_MODELS (a, setAlpha_nnet (n, alpha));
}

void setAlphaKnee_nnr (NNR a, double knee_db)
{
	NNR_ALL_MODELS (a, setKnee_nnet (n, knee_db));
}

void setTau_nnr (NNR a, double tau)
{
	NNR_ALL_MODELS (a, setTau_nnet (n, tau));
}

void setMaxGain_nnr (NNR a, double gmax_db)
{
	NNR_ALL_MODELS (a, setMaxGain_nnet (n, gmax_db));
}

void setSmooth_nnr (NNR a, double att_ms, double rel_ms)
{
	NNR_ALL_MODELS (a, setSmooth_nnet (n, att_ms, rel_ms));
}

int getPosition_nnr (NNR a)
{
	return a->position;
}

int getNNRTrainingBand (NNR a, double* band_low, double* band_high)
{
	NNET n = a->nets[a->model];
	*band_low = *band_high = 0.0;
	if (n == 0) return 0;
	return getTrainingBand (n, band_low, band_high);
}

/********************************************************************************************************
*																										*
*									   Training BW & NNR Bypass											*
*																										*
********************************************************************************************************/


void setNNRbypassRun (int channel)
{
	double band_low = 0.0, band_high = 0.0;
	NNR a = rxa[channel].nnr.p;
	NNRBYPASS b = rxa[channel].nnrbypass.p;
	int was = getRun_nnrbypass (b);
	if (getRun_nnr (a) &&
		getNNRTrainingBand (a, &band_low, &band_high))
	{
		switch (rxa[channel].mode)
		{
			case RXA_CWU:
			case RXA_USB:
			case RXA_DIGU:
				setRun_nnrbypass (b, 1, +band_low, +band_high);
				break;
			case RXA_CWL:
			case RXA_LSB:
			case RXA_DIGL:
				setRun_nnrbypass (b, 1, -band_high, -band_low);
				break;
			default:
				setRun_nnrbypass (b, 0, band_low, band_high);
				break;
		}
	}
	else   setRun_nnrbypass (b, 0,  band_low,  band_high);

	if (was && !getRun_nnrbypass(b))
	{
		flush_bpsnba(rxa[channel].bpsnba.p);
		flush_snba(rxa[channel].snba.p);
		flush_eqp(rxa[channel].eqp.p);
		if (rxa[channel].anf.p->position == 0) flush_anf(rxa[channel].anf.p);
		if (rxa[channel].anr.p->position == 0) flush_anr(rxa[channel].anr.p);
		if (rxa[channel].emnr.p->position == 0) flush_emnr(rxa[channel].emnr.p);
	}

	{
		int position = getRun_nnrbypass (b) ? 0 : 1;
		if (a->position != position)
		{
			a->position = position;
			flush_nnr (a);
		}
	}

	RXAbp1Check (channel, 
		rxa[channel].amd.p->run, rxa[channel].snba.p->run,
		rxa[channel].emnr.p->run, getRun_nnrbypass (b), 
		rxa[channel].anf.p->run, rxa[channel].anr.p->run);
	RXAbp1Set (channel);
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT void
SetRXANNRRun (int channel, int setit)
{
	if (rxa[channel].nnr.p->run != setit)
	{
		EnterCriticalSection (&ch[channel].csDSP);
		rxa[channel].nnr.p->run = setit;
		flush_nnr (rxa[channel].nnr.p);
		setNNRbypassRun (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

PORT void
SetRXANNRPosition (int channel, int position)
{
	(void)channel;
	(void)position;
}

// Major user control - Sets the minimum gain which effectively sets
//    the amount of pass-through signal/noise for user comfort.
PORT void
SetRXANNRMaskFloor (int channel, double floor_db)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].nnr.p->mask_floor = floor_db;
	NNR_ALL_MODELS (rxa[channel].nnr.p, setFloor_nnet (n, floor_db));
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Select which model to use -- selects the 'slot'.
PORT int
SetRXANNRModel (int channel, int slot)
{
	int now;
	EnterCriticalSection (&ch[channel].csDSP);
	now = setModel_nnr (rxa[channel].nnr.p, slot);
	setNNRbypassRun(channel);
	LeaveCriticalSection (&ch[channel].csDSP);
	return now;
}

// Returns the model currently in use.
PORT int
GetRXANNRModel (int channel)
{
	int now;
	EnterCriticalSection (&ch[channel].csDSP);
	now = getModel_nnr (rxa[channel].nnr.p);
	LeaveCriticalSection (&ch[channel].csDSP);
	return now;
}

// 'cmode == 0' returns audio in both I and Q.
// 'cmode != 0' returns audio in I and '0.0' in Q.
PORT void
SetRXANNRcmode (int channel, int cmode)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].nnr.p->cmode = cmode;
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRTestMode (int channel, int mode)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setMode_nnet (n, mode));
	flush_nnr (rxa[channel].nnr.p);
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRAlpha (int channel, double alpha)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setAlpha_nnet (n, alpha));
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRAlphaKnee (int channel, double knee_db)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setKnee_nnet (n, knee_db));
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRTau (int channel, double tau)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setTau_nnet (n, tau));
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRMaxGain (int channel, double gmax_db)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setMaxGain_nnet (n, gmax_db));
	LeaveCriticalSection (&ch[channel].csDSP);
}

// Used in testing; not intended as a user control.
PORT void
SetRXANNRSmooth (int channel, double att_ms, double rel_ms)
{
	EnterCriticalSection (&ch[channel].csDSP);
	NNR_ALL_MODELS (rxa[channel].nnr.p, setSmooth_nnet (n, att_ms, rel_ms));
	LeaveCriticalSection (&ch[channel].csDSP);
}


