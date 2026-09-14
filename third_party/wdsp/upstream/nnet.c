/*  nnet.c

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

warren@wpratt.com

*/

#define _CRT_SECURE_NO_WARNINGS

#include "comm.h"
#include "nnet_profile.h"
#include "nnet.h"
#include "nnio.h"

#define NNET_NSTAGE		10
#define NNET_KT			2
#define NNET_KF			5
#define NNET_SF			2
#define NNET_PF			2
#define NNET_GMAX_DB	12.0

#define NNR_PI			3.14159265358979323846
#define NNR_TWOPI		6.28318530717958647692
#define DF_TINY			1.0e-30
#define COND_TINY		1.0e-20

#define NNET_TAU_DEFAULT	2.0
#define NNET_FRAME_RATE		62.5

static char nnet_model_path[NNET_NSLOTS][512] =
{
	"wdsp_nnr_0.bin",
#if NNET_NSLOTS > 1
	"wdsp_nnr_1.bin",
#endif
#if NNET_NSLOTS > 2
	"wdsp_nnr_2.bin",
#endif
#if NNET_NSLOTS > 3
	"wdsp_nnr_3.bin",
#endif
};


extern const unsigned char nnr_model_0_data[];
extern const unsigned int  nnr_model_0_size;
#if NNET_NSLOTS > 1
extern const unsigned char nnr_model_1_data[];
extern const unsigned int  nnr_model_1_size;
#endif
#if NNET_NSLOTS > 2
extern const unsigned char nnr_model_2_data[];
extern const unsigned int  nnr_model_2_size;
#endif
#if NNET_NSLOTS > 3
extern const unsigned char nnr_model_3_data[];
extern const unsigned int  nnr_model_3_size;
#endif
#if NNET_NSLOTS > 4
#error "add the extern declarations and table entries for slots 4 and up"
#endif

static const struct
{
	const unsigned char* data;
	const unsigned int*  size;
} nnr_builtin[NNET_NSLOTS] =
{
	{ nnr_model_0_data, &nnr_model_0_size },
#if NNET_NSLOTS > 1
	{ nnr_model_1_data, &nnr_model_1_size },
#endif
#if NNET_NSLOTS > 2
	{ nnr_model_2_data, &nnr_model_2_size },
#endif
#if NNET_NSLOTS > 3
	{ nnr_model_3_data, &nnr_model_3_size },
#endif
};


typedef struct _gru
{
	int nin;
	int nhidden;
	const double* w_ih;
	const double* w_hh;
	const double* b_ih;
	const double* b_hh;
	double* h;
	double* gi;
	double* gh;
} gru;				/* GRU is typedef'd in nnet.h */

static double gru_sigmoid (double x)
{
	if (x >= 0.0) return 1.0 / (1.0 + exp (-x));
	else
	{
		double e = exp (x);
		return e / (1.0 + e);
	}
}

GRU create_gru
	(
	int nin,
	int nhidden,
	const double* w_ih,
	const double* w_hh,
	const double* b_ih,
	const double* b_hh
	)
{
	GRU g = (GRU) malloc0 (sizeof (gru));
	g->nin     = nin;
	g->nhidden = nhidden;
	g->w_ih    = w_ih;
	g->w_hh    = w_hh;
	g->b_ih    = b_ih;
	g->b_hh    = b_hh;
	g->h  = (double*) malloc0 (    nhidden * sizeof (double));
	g->gi = (double*) malloc0 (3 * nhidden * sizeof (double));
	g->gh = (double*) malloc0 (3 * nhidden * sizeof (double));
	return g;
}

void destroy_gru (GRU g)
{
	_aligned_free (g->gh);
	_aligned_free (g->gi);
	_aligned_free (g->h);
	_aligned_free (g);
}

void flush_gru (GRU g)
{
	memset (g->h, 0, g->nhidden * sizeof (double));
}

const double* run_gru_dbg
	(
	GRU g,
	const double* x,
	double* rout,
	double* zout,
	double* nout
	)
{
	int i, j;
	const int I = g->nin;
	const int H = g->nhidden;
	const int H2 = 2 * H;
	const int H3 = 3 * H;
	const double* W = g->w_ih;
	const double* U = g->w_hh;
	double* gi = g->gi;
	double* gh = g->gh;
	double* h = g->h;
	double acc, r, z, n;

	for (i = 0; i < H3; i++)
	{
		const double* row = W + (size_t)i * I;
		acc = g->b_ih[i];
		for (j = 0; j < I; j++) acc += row[j] * x[j];
		gi[i] = acc;
	}

	for (i = 0; i < H3; i++)
	{
		const double* row = U + (size_t)i * H;
		acc = g->b_hh[i];
		for (j = 0; j < H; j++) acc += row[j] * h[j];
		gh[i] = acc;
	}

	for (i = 0; i < H; i++)
	{
		r = gru_sigmoid (gi[i]      + gh[i]);
		z = gru_sigmoid (gi[H + i]  + gh[H + i]);
		n = tanh        (gi[H2 + i] + r * gh[H2 + i]);

		if (rout) rout[i] = r;
		if (zout) zout[i] = z;
		if (nout) nout[i] = n;

		h[i] = (1.0 - z) * n + z * h[i];
	}

	return h;
}

const double* run_gru (GRU g, const double* x)
{
	return run_gru_dbg (g, x, 0, 0, 0);
}

const double* getState_gru (GRU g)
{
	return g->h;
}

void setState_gru (GRU g, const double* h)
{
	memcpy (g->h, h, g->nhidden * sizeof (double));
}

typedef struct _conv2d
{
	int cin;
	int cout;
	int fin;
	int fout;
	int kt;
	int kf;
	int stride_f;
	int pad_f;
	int slotw;
	const double* w;
	const double* b;
	double* ring;
	int head;
	double* y;
} conv2d;				/* CONV2D is typedef'd in nnet.h */

CONV2D create_conv2d
	(
	int cin,
	int cout,
	int fin,
	int kt,
	int kf,
	int stride_f,
	int pad_f,
	const double* w,
	const double* b
	)
{
	CONV2D c = (CONV2D) malloc0 (sizeof (conv2d));
	c->cin      = cin;
	c->cout     = cout;
	c->fin      = fin;
	c->kt       = kt;
	c->kf       = kf;
	c->stride_f = stride_f;
	c->pad_f    = pad_f;
	c->w        = w;
	c->b        = b;

	c->fout  = (fin + 2 * pad_f - kf) / stride_f + 1;
	c->slotw = fin + 2 * pad_f;

	if (c->fout < 1 || (c->fout - 1) * stride_f + kf > c->slotw)
		dprintf ("conv2d: bad geometry cin=%d cout=%d fin=%d kt=%d kf=%d sf=%d pf=%d\n",
			cin, cout, fin, kt, kf, stride_f, pad_f);

	c->ring = (double*) malloc0 ((size_t)kt * cin * c->slotw * sizeof (double));
	c->y    = (double*) malloc0 ((size_t)cout * c->fout      * sizeof (double));
	c->head = 0;
	return c;
}

void destroy_conv2d (CONV2D c)
{
	_aligned_free (c->y);
	_aligned_free (c->ring);
	_aligned_free (c);
}

void flush_conv2d (CONV2D c)
{
	memset (c->ring, 0, (size_t)c->kt * c->cin * c->slotw * sizeof (double));
	c->head = 0;
}

const double* run_conv2d (CONV2D c, const double* x)
{
	const int Cin = c->cin, Cout = c->cout;
	const int Fin = c->fin, Fout = c->fout;
	const int kt = c->kt, kf = c->kf;
	const int sf = c->stride_f, pf = c->pad_f;
	const int W = c->slotw;
	int co, ci, i, f, j, s;
	double acc;
	double* slot;

	c->head = (c->head + 1) % kt;
	slot = c->ring + (size_t)c->head * Cin * W;
	for (ci = 0; ci < Cin; ci++)
		memcpy (slot + (size_t)ci * W + pf, x + (size_t)ci * Fin, Fin * sizeof (double));

	for (co = 0; co < Cout; co++)
	{
		const double* wco = c->w + (size_t)co * Cin * kt * kf;
		double* yco = c->y + (size_t)co * Fout;

		for (f = 0; f < Fout; f++) yco[f] = c->b[co];

		for (i = 0; i < kt; i++)
		{
			s = (c->head + 1 + i) % kt;
			for (ci = 0; ci < Cin; ci++)
			{
				const double* wk = wco + ((size_t)ci * kt + i) * kf;
				const double* xr = c->ring + ((size_t)s * Cin + ci) * W;
				for (f = 0; f < Fout; f++)
				{
					const double* xp = xr + (size_t)f * sf;
					acc = 0.0;
					for (j = 0; j < kf; j++) acc += wk[j] * xp[j];
					yco[f] += acc;
				}
			}
		}
	}
	return c->y;
}

int getFout_conv2d (CONV2D c)
{
	return c->fout;
}

int getCout_conv2d (CONV2D c)
{
	return c->cout;
}

void run_prelu (double* x, const double* slope, int nch, int nfreq)
{
	int k, f;
	for (k = 0; k < nch; k++)
	{
		double a = slope[k];
		double* r = x + (size_t)k * nfreq;
		for (f = 0; f < nfreq; f++)
			if (r[f] < 0.0) r[f] *= a;
	}
}

typedef struct _convt2d
{
	int cin;
	int cout;
	int fin;
	int fout;
	int kt;
	int kf;
	int stride_f;
	int pad_f;
	int opad_f;
	int maxtaps;
	int* ntaps;
	int* slo;
	int* shi;
	int* tap_j;
	int* tap_i;
	const double* w;
	const double* b;
	double* ring;
	int head;
	double* y;
} convt2d;				/* CONVT2D is typedef'd in nnet.h */

CONVT2D create_convt2d
	(
	int cin,
	int cout,
	int fin,
	int kt,
	int kf,
	int stride_f,
	int pad_f,
	int opad_f,
	const double* w,
	const double* b
	)
{
	CONVT2D c = (CONVT2D) malloc0 (sizeof (convt2d));
	int f, j, num, fi, n;

	c->cin      = cin;
	c->cout     = cout;
	c->fin      = fin;
	c->kt       = kt;
	c->kf       = kf;
	c->stride_f = stride_f;
	c->pad_f    = pad_f;
	c->opad_f   = opad_f;
	c->w        = w;
	c->b        = b;

	c->fout    = (fin - 1) * stride_f - 2 * pad_f + kf + opad_f;
	c->maxtaps = (kf + stride_f - 1) / stride_f;

	if (c->fout < 1)
		dprintf ("convt2d: bad geometry cin=%d cout=%d fin=%d kt=%d kf=%d sf=%d pf=%d opf=%d\n",
			cin, cout, fin, kt, kf, stride_f, pad_f, opad_f);

	c->ntaps = (int*) malloc0 ((size_t)c->fout * sizeof (int));
	c->tap_j = (int*) malloc0 ((size_t)c->fout * c->maxtaps * sizeof (int));
	c->tap_i = (int*) malloc0 ((size_t)c->fout * c->maxtaps * sizeof (int));

	for (f = 0; f < c->fout; f++)
	{
		n = 0;
		for (j = 0; j < kf; j++)
		{
			num = f + pad_f - j;
			if (num < 0) continue;
			if (num % stride_f) continue;
			fi = num / stride_f;
			if (fi >= fin) continue;
			c->tap_j[(size_t)f * c->maxtaps + n] = j;
			c->tap_i[(size_t)f * c->maxtaps + n] = fi;
			n++;
		}
		c->ntaps[f] = n;
	}

	c->slo = (int*) malloc0 ((size_t)kf * sizeof (int));
	c->shi = (int*) malloc0 ((size_t)kf * sizeof (int));
	for (j = 0; j < kf; j++)
	{
		int lo = 0, hi = fin - 1;
		while (lo <= hi && (lo * stride_f - pad_f + j) <  0)       lo++;
		while (hi >= lo && (hi * stride_f - pad_f + j) >= c->fout) hi--;
		c->slo[j] = lo;
		c->shi[j] = hi;
	}

	c->ring = (double*) malloc0 ((size_t)kt * cin * fin * sizeof (double));
	c->y    = (double*) malloc0 ((size_t)cout * c->fout  * sizeof (double));
	c->head = 0;
	return c;
}

void destroy_convt2d (CONVT2D c)
{
	_aligned_free (c->y);
	_aligned_free (c->ring);
	_aligned_free (c->tap_i);
	_aligned_free (c->tap_j);
	_aligned_free (c->ntaps);
	_aligned_free (c->slo);
	_aligned_free (c->shi);
	_aligned_free (c);
}

void flush_convt2d (CONVT2D c)
{
	memset (c->ring, 0, (size_t)c->kt * c->cin * c->fin * sizeof (double));
	c->head = 0;
}

const double* run_convt2d (CONVT2D c, const double* x)
{
	const int Cin = c->cin, Cout = c->cout;
	const int Fin = c->fin, Fout = c->fout;
	const int kt = c->kt, kf = c->kf;
	const int sf = c->stride_f, pf = c->pad_f;
	int co, ci, i, f, j, s;

	c->head = (c->head + 1) % kt;
	memcpy (c->ring + (size_t)c->head * Cin * Fin, x,
		(size_t)Cin * Fin * sizeof (double));

	for (co = 0; co < Cout; co++)
	{
		double* yco = c->y + (size_t)co * Fout;
		for (f = 0; f < Fout; f++) yco[f] = c->b[co];
	}

	for (i = 0; i < kt; i++)
	{
		s = (c->head - i + kt) % kt;
		for (ci = 0; ci < Cin; ci++)
		{
			const double* xr = c->ring + ((size_t)s * Cin + ci) * Fin;
			for (co = 0; co < Cout; co++)
			{
				const double* wk = c->w + (((size_t)ci * Cout + co) * kt + i) * kf;
				double* yco = c->y + (size_t)co * Fout;
				for (j = 0; j < kf; j++)
				{
					const double wv = wk[j];
					const int lo = c->slo[j], hi = c->shi[j];
					const int off = j - pf;
					for (f = lo; f <= hi; f++)
						yco[f * sf + off] += wv * xr[f];
				}
			}
		}
	}
	return c->y;
}

int getFout_convt2d (CONVT2D c)
{
	return c->fout;
}

int getCout_convt2d (CONVT2D c)
{
	return c->cout;
}

typedef struct _dfhead
{
	int nbins;
	int order;
	double gmin;
	double gmax;
	double alpha;
	double knee;
	double a_att;
	double a_rel;
	double* gprev;
	double* ring_re;
	double* ring_im;
	int head;
} dfhead;				/* DFHEAD is typedef'd in nnet.h */

DFHEAD create_dfhead (int nbins, int order, double gmin_db, double gmax_db)
{
	int i;
	DFHEAD d = (DFHEAD) malloc0 (sizeof (dfhead));
	d->nbins = nbins;
	d->order = order;
	d->gmin  = pow (10.0, gmin_db / 20.0);
	d->gmax  = pow (10.0, gmax_db / 20.0);
	d->alpha = 1.0;
	d->knee  = pow (10.0, -10.0 / 20.0);		// -10 dB
	d->a_att = 0.0;
	d->a_rel = 0.0;
	d->gprev = (double*) malloc0 (nbins * sizeof (double));
	for (i = 0; i < nbins; i++) d->gprev[i] = 1.0;
	d->ring_re = (double*) malloc0 ((size_t)order * nbins * sizeof (double));
	d->ring_im = (double*) malloc0 ((size_t)order * nbins * sizeof (double));
	d->head = 0;
	return d;
}

void destroy_dfhead (DFHEAD d)
{
	_aligned_free (d->gprev);
	_aligned_free (d->ring_im);
	_aligned_free (d->ring_re);
	_aligned_free (d);
}

void flush_dfhead (DFHEAD d)
{
	if (d->gprev)
	{
		int q;
		for (q = 0; q < d->nbins; q++) d->gprev[q] = 1.0;
	}
	memset (d->ring_re, 0, (size_t)d->order * d->nbins * sizeof (double));
	memset (d->ring_im, 0, (size_t)d->order * d->nbins * sizeof (double));
	d->head = 0;
}

void run_dfhead
	(
	DFHEAD d,
	const double* xre,
	const double* xim,
	const double* coef,
	double* yre,
	double* yim
	)
{
	const int K = d->nbins;
	const int P = d->order;
	int i, k, s;
	double cr, ci, xr, xi, ax, ay, g, sc;
	double* cur_re;
	double* cur_im;

	d->head = (d->head + 1) % P;
	cur_re = d->ring_re + (size_t)d->head * K;
	cur_im = d->ring_im + (size_t)d->head * K;
	memcpy (cur_re, xre, K * sizeof (double));
	memcpy (cur_im, xim, K * sizeof (double));

	memset (yre, 0, K * sizeof (double));
	memset (yim, 0, K * sizeof (double));

	for (i = 0; i < P; i++)
	{
		const double* rr;
		const double* ri;
		const double* pcr = coef + (size_t)(2 * i + 0) * K;
		const double* pci = coef + (size_t)(2 * i + 1) * K;
		s = (d->head - i + P) % P;
		rr = d->ring_re + (size_t)s * K;
		ri = d->ring_im + (size_t)s * K;

		for (k = 0; k < K; k++)
		{
			cr = pcr[k];  ci = pci[k];
			xr = rr[k];   xi = ri[k];
			yre[k] += cr * xr - ci * xi;
			yim[k] += cr * xi + ci * xr;
		}
	}

	for (k = 0; k < K; k++)
	{
		xr = cur_re[k];  xi = cur_im[k];
		ax = sqrt (xr * xr + xi * xi);
		if (ax <= DF_TINY) continue;

		ay = sqrt (yre[k] * yre[k] + yim[k] * yim[k]);
		g = ay / ax;

		if (d->alpha != 1.0 && g > DF_TINY && g < d->knee)
		{
			sc = pow (g / d->knee, d->alpha - 1.0);
			yre[k] *= sc;
			yim[k] *= sc;
			g *= sc;
		}

		if (d->a_att > 0.0 || d->a_rel > 0.0)
		{
			double gp = d->gprev[k];
			double cf = (g > gp) ? d->a_att : d->a_rel;
			double gs = cf * gp + (1.0 - cf) * g;
			d->gprev[k] = gs;
			if (g > DF_TINY)
			{
				sc = gs / g;
				yre[k] *= sc;
				yim[k] *= sc;
				g = gs;
			}
		}
		else
			d->gprev[k] = g;

		if (g > d->gmax)
		{
			sc = d->gmax / g;
			yre[k] *= sc;
			yim[k] *= sc;
		}
		else if (g < d->gmin)
		{
			if (ay > DF_TINY)
			{
				sc = d->gmin / g;
				yre[k] *= sc;
				yim[k] *= sc;
			}
			else
			{
				yre[k] = d->gmin * xr;
				yim[k] = d->gmin * xi;
			}
		}
	}
}

void setGains_dfhead (DFHEAD d, double gmin_db, double gmax_db)
{
	d->gmin = pow (10.0, gmin_db / 20.0);
	d->gmax = pow (10.0, gmax_db / 20.0);
}

void setAlpha_dfhead (DFHEAD d, double alpha)
{
	if (alpha < 0.0) alpha = 0.0;
	if (alpha > 4.0) alpha = 4.0;
	d->alpha = alpha;
}

double getAlpha_dfhead (DFHEAD d)
{
	return d->alpha;
}

void setKnee_dfhead (DFHEAD d, double knee_db)
{
	if (knee_db < 0.0)  knee_db = 0.0;
	if (knee_db > 40.0) knee_db = 40.0;
	d->knee = pow (10.0, -knee_db / 20.0);
}

double getKnee_dfhead (DFHEAD d)
{
	return -20.0 * log10 (d->knee);
}

void setSmooth_dfhead (DFHEAD d, double att_ms, double rel_ms,
	double frame_rate)
{
	if (att_ms < 0.0) att_ms = 0.0;
	if (rel_ms < 0.0) rel_ms = 0.0;
	if (att_ms > 500.0) att_ms = 500.0;
	if (rel_ms > 500.0) rel_ms = 500.0;
	d->a_att = (att_ms > 0.0)
		? exp (-1000.0 / (att_ms * frame_rate)) : 0.0;
	d->a_rel = (rel_ms > 0.0)
		? exp (-1000.0 / (rel_ms * frame_rate)) : 0.0;
}

int getOrder_dfhead (DFHEAD d)
{
	return d->order;
}

typedef struct _cond
{
	int nbins;
	double cexp;
	double pexp;
	double tau;
	double frame_rate;
	double alpha;
	double pfloor;
	double pstate;
	double wstate;
	double gain;
	double* out;
} cond;				/* COND is typedef'd in nnet.h */

static void calc_cond (COND c)
{
	c->pexp  = c->cexp - 1.0;
	c->alpha = exp (-1.0 / (c->tau * c->frame_rate));
}

COND create_cond
	(
	int nbins,
	double cexp,
	double tau,
	double frame_rate,
	double pfloor
	)
{
	COND c = (COND) malloc0 (sizeof (cond));
	c->nbins      = nbins;
	c->cexp       = cexp;
	c->tau        = tau;
	c->frame_rate = frame_rate;
	c->pfloor     = pfloor;
	c->out        = (double*) malloc0 (2 * (size_t)nbins * sizeof (double));
	calc_cond (c);
	flush_cond (c);
	return c;
}

void destroy_cond (COND c)
{
	_aligned_free (c->out);
	_aligned_free (c);
}

void flush_cond (COND c)
{
	c->pstate = 0.0;
	c->wstate = 0.0;
	c->gain   = 1.0;
	memset (c->out, 0, 2 * (size_t)c->nbins * sizeof (double));
}

const double* run_cond (COND c, const double* xre, const double* xim)
{
	const int K = c->nbins;
	double* orr = c->out;
	double* oii = c->out + K;
	double mag, s, pw, phat, g;
	int k;

	pw = 0.0;
	for (k = 0; k < K; k++)
	{
		mag = sqrt (xre[k] * xre[k] + xim[k] * xim[k]);
		if (mag > COND_TINY)
		{
			s = pow (mag, c->pexp);
			orr[k] = xre[k] * s;
			oii[k] = xim[k] * s;
		}
		else
		{
			orr[k] = 0.0;
			oii[k] = 0.0;
		}
		pw += orr[k] * orr[k] + oii[k] * oii[k];
	}
	pw /= (double)K;

	c->pstate = c->alpha * c->pstate + (1.0 - c->alpha) * pw;
	c->wstate = c->alpha * c->wstate + (1.0 - c->alpha);

	phat = (c->wstate > COND_TINY) ? c->pstate / c->wstate : pw;
	if (phat < c->pfloor) phat = c->pfloor;

	g = 1.0 / sqrt (phat);
	c->gain = g;

	for (k = 0; k < K; k++)
	{
		orr[k] *= g;
		oii[k] *= g;
	}

	return c->out;
}

double getGain_cond (COND c)
{
	return c->gain;
}

void setTau_cond (COND c, double tau)
{
	c->tau = tau;
	calc_cond (c);
}

typedef struct _dprnn
{
	int nch;
	int nfreq;
	int hid;
	int ok;

	GRU ifwd;
	GRU ibwd;
	GRU* tgru;

	const double* ilin_w;
	const double* ilin_b;
	const double* tlin_w;
	const double* tlin_b;

	double* u;
	double* cat;
	double* lin;
	double* y;
} dprnn;				/* DPRNN is typedef'd in nnet.h */

static void dprnn_linear
	(
	double* y,
	const double* w,
	const double* b,
	const double* x,
	int nin,
	int nout
	)
{
	int i, j;
	double acc;
	for (i = 0; i < nout; i++)
	{
		const double* row = w + (size_t)i * nin;
		acc = b[i];
		for (j = 0; j < nin; j++) acc += row[j] * x[j];
		y[i] = acc;
	}
}

static const double* dp_get (NNIO f, const char* prefix, const char* suffix,
	int d0, int d1, int* ok)
{
	char nm[80];
	const double* p;
	sprintf (nm, "%s%s", prefix, suffix);
	p = nnio_get (f, nm, d0, d1, 0, 0);
	if (p == 0) *ok = 0;
	return p;
}

DPRNN create_dprnn (NNIO f, const char* prefix, int nch, int nfreq, int hid)
{
	DPRNN d = (DPRNN) malloc0 (sizeof (dprnn));
	const double *fw, *fu, *fbi, *fbh;
	const double *bw, *bu, *bbi, *bbh;
	const double *tw, *tu, *tbi, *tbh;
	int i;

	d->nch   = nch;
	d->nfreq = nfreq;
	d->hid   = hid;
	d->ok    = 1;

	fw  = dp_get (f, prefix, "_ifwd_wih", 3 * hid, nch, &d->ok);
	fu  = dp_get (f, prefix, "_ifwd_whh", 3 * hid, hid, &d->ok);
	fbi = dp_get (f, prefix, "_ifwd_bih", 3 * hid, 0,   &d->ok);
	fbh = dp_get (f, prefix, "_ifwd_bhh", 3 * hid, 0,   &d->ok);

	bw  = dp_get (f, prefix, "_ibwd_wih", 3 * hid, nch, &d->ok);
	bu  = dp_get (f, prefix, "_ibwd_whh", 3 * hid, hid, &d->ok);
	bbi = dp_get (f, prefix, "_ibwd_bih", 3 * hid, 0,   &d->ok);
	bbh = dp_get (f, prefix, "_ibwd_bhh", 3 * hid, 0,   &d->ok);

	tw  = dp_get (f, prefix, "_tgru_wih", 3 * nch, nch, &d->ok);
	tu  = dp_get (f, prefix, "_tgru_whh", 3 * nch, nch, &d->ok);
	tbi = dp_get (f, prefix, "_tgru_bih", 3 * nch, 0,   &d->ok);
	tbh = dp_get (f, prefix, "_tgru_bhh", 3 * nch, 0,   &d->ok);

	d->ilin_w = dp_get (f, prefix, "_ilin_w", nch, 2 * hid, &d->ok);
	d->ilin_b = dp_get (f, prefix, "_ilin_b", nch, 0,       &d->ok);
	d->tlin_w = dp_get (f, prefix, "_tlin_w", nch, nch,     &d->ok);
	d->tlin_b = dp_get (f, prefix, "_tlin_b", nch, 0,       &d->ok);

	if (!d->ok)
	{
		dprintf ("dprnn: '%s' is missing tensors\n", prefix);
		return d;
	}

	d->ifwd = create_gru (nch, hid, fw, fu, fbi, fbh);
	d->ibwd = create_gru (nch, hid, bw, bu, bbi, bbh);

	d->tgru = (GRU*) malloc0 ((size_t)nfreq * sizeof (GRU));
	for (i = 0; i < nfreq; i++)
		d->tgru[i] = create_gru (nch, nch, tw, tu, tbi, tbh);

	d->u   = (double*) malloc0 ((size_t)nfreq * nch     * sizeof (double));
	d->cat = (double*) malloc0 ((size_t)nfreq * 2 * hid * sizeof (double));
	d->lin = (double*) malloc0 ((size_t)nch             * sizeof (double));
	d->y   = (double*) malloc0 ((size_t)nch * nfreq     * sizeof (double));
	return d;
}

void destroy_dprnn (DPRNN d)
{
	int i;
	if (d->ok)
	{
		for (i = 0; i < d->nfreq; i++) destroy_gru (d->tgru[i]);
		_aligned_free (d->tgru);
		destroy_gru (d->ibwd);
		destroy_gru (d->ifwd);
		_aligned_free (d->y);
		_aligned_free (d->lin);
		_aligned_free (d->cat);
		_aligned_free (d->u);
	}
	_aligned_free (d);
}

void flush_dprnn (DPRNN d)
{
	int i;
	if (!d->ok) return;
	for (i = 0; i < d->nfreq; i++) flush_gru (d->tgru[i]);
	flush_gru (d->ifwd);
	flush_gru (d->ibwd);
}

const double* run_dprnn (DPRNN d, const double* x)
{
	const int C = d->nch;
	const int F = d->nfreq;
	const int H = d->hid;
	int f, c;
	const double* h;

	if (!d->ok) return x;

	for (c = 0; c < C; c++)
		for (f = 0; f < F; f++)
			d->u[(size_t)f * C + c] = x[(size_t)c * F + f];

	flush_gru (d->ifwd);
	flush_gru (d->ibwd);

	for (f = 0; f < F; f++)
	{
		h = run_gru (d->ifwd, d->u + (size_t)f * C);
		memcpy (d->cat + (size_t)f * 2 * H, h, H * sizeof (double));
	}
	for (f = F - 1; f >= 0; f--)
	{
		h = run_gru (d->ibwd, d->u + (size_t)f * C);
		memcpy (d->cat + (size_t)f * 2 * H + H, h, H * sizeof (double));
	}

	for (f = 0; f < F; f++)
	{
		double* ur = d->u + (size_t)f * C;
		dprnn_linear (d->lin, d->ilin_w, d->ilin_b,
			d->cat + (size_t)f * 2 * H, 2 * H, C);
		for (c = 0; c < C; c++) ur[c] += d->lin[c];
	}

	// ---- inter: GRU across time, independent state at each frequency
	for (f = 0; f < F; f++)
	{
		double* ur = d->u + (size_t)f * C;
		h = run_gru (d->tgru[f], ur);
		dprnn_linear (d->lin, d->tlin_w, d->tlin_b, h, C, C);
		for (c = 0; c < C; c++) ur[c] += d->lin[c];
	}

	for (c = 0; c < C; c++)
		for (f = 0; f < F; f++)
			d->y[(size_t)c * F + f] = d->u[(size_t)f * C + c];

	return d->y;
}

int ok_dprnn (DPRNN d)
{
	return d->ok;
}

typedef struct _nnet
{
	int slot;
	int nbins;
	int lookahead;
	double tau;
	double gmax_db;
	double sm_att;
	double sm_rel;
	double floor_db;
	int ready;
	int mode;

	int c[4];
	int nf[5];
	int hid;
	int order;

	NNIO mf;
	COND cnd;
	DFHEAD df;

	CONV2D  enc[4];
	DPRNN   dp[2];
	CONVT2D dec[4];

	const double* enc_slope[4];
	const double* dec_slope[3];

	double* e[4];
	double* p[2];
	double* d[4];
	double* cat[4];
} nnet;				/* NNET is typedef'd in nnet.h */

static int nnet_build (NNET n)
{
	const double* cfg;
	char nm[64];
	int i, want, cin, cout, nf;

	if (n->slot < 0 || n->slot >= NNET_NSLOTS) n->slot = 0;

	if (nnet_model_path[n->slot][0] != '\0' &&
		(n->mf = nnio_open (nnet_model_path[n->slot])) != 0)
		dprintf ("nnet: slot %d using the model file '%s'\n",
				 n->slot, nnet_model_path[n->slot]);
	else if (*nnr_builtin[n->slot].size > 0 &&
			 (n->mf = nnio_open_mem (nnr_builtin[n->slot].data,
									 *nnr_builtin[n->slot].size,
									 "a built-in model")) != 0)
		dprintf ("nnet: no '%s' in the working directory - slot %d is using "
				 "its built-in model\n", nnet_model_path[n->slot], n->slot);
	else
	{
		dprintf ("nnet: slot %d is empty - no '%s' and nothing built in\n",
				 n->slot, nnet_model_path[n->slot]);
		return 0;
	}

	if (n->mf == 0) return 0;
	if ((cfg = nnio_get (n->mf, "cfg", 7, 0, 0, 0)) == 0) return 0;

	if ((int)cfg[0] != n->nbins)
	{
		dprintf ("nnet: model is for %d bins, framework wants %d\n",
			(int)cfg[0], n->nbins);
		return 0;
	}
	n->c[0]  = (int)cfg[1];
	n->c[1]  = (int)cfg[2];
	n->c[2]  = (int)cfg[3];
	n->c[3]  = (int)cfg[4];
	n->hid   = (int)cfg[5];
	n->order = (int)cfg[6];

	if (nnio_find (n->mf, "lookahead") >= 0)
	{
		const double* la = nnio_get (n->mf, "lookahead", 1, 0, 0, 0);
		if (la) n->lookahead = (int) la[0];
	}

	n->nf[0] = n->nbins;
	for (i = 0; i < 4; i++)
	{
		const double *w, *b;
		cin = (i == 0) ? 2 : n->c[i - 1];

		sprintf (nm, "enc%d_w", i + 1);
		w = nnio_get (n->mf, nm, n->c[i], cin, NNET_KT, NNET_KF);
		sprintf (nm, "enc%d_b", i + 1);
		b = nnio_get (n->mf, nm, n->c[i], 0, 0, 0);
		sprintf (nm, "enc%d_slope", i + 1);
		n->enc_slope[i] = nnio_get (n->mf, nm, n->c[i], 0, 0, 0);
		if (!w || !b || !n->enc_slope[i]) return 0;

		n->enc[i] = create_conv2d (cin, n->c[i], n->nf[i],
			NNET_KT, NNET_KF, NNET_SF, NNET_PF, w, b);
		n->nf[i + 1] = getFout_conv2d (n->enc[i]);
		n->e[i] = (double*) malloc0
			((size_t)n->c[i] * n->nf[i + 1] * sizeof (double));
	}

	for (i = 0; i < 2; i++)
	{
		sprintf (nm, "dp%d", i + 1);
		n->dp[i] = create_dprnn (n->mf, nm, n->c[3], n->nf[4], n->hid);
		if (!ok_dprnn (n->dp[i])) return 0;
		n->p[i] = (double*) malloc0
			((size_t)n->c[3] * n->nf[4] * sizeof (double));
	}

	for (i = 0; i < 4; i++)
	{
		const double *w, *b;
		int cup  = (i == 0) ? n->c[3] : n->c[3 - i];
		int cskp = n->c[3 - i];

		cin  = cup + cskp;
		cout = (i == 3) ? 2 * n->order : n->c[2 - i];
		nf   = n->nf[4 - i];

		sprintf (nm, "dec%d_w", i + 1);
		w = nnio_get (n->mf, nm, cin, cout, NNET_KT, NNET_KF);
		sprintf (nm, "dec%d_b", i + 1);
		b = nnio_get (n->mf, nm, cout, 0, 0, 0);
		if (!w || !b) return 0;

		if (i < 3)
		{
			sprintf (nm, "dec%d_slope", i + 1);
			n->dec_slope[i] = nnio_get (n->mf, nm, cout, 0, 0, 0);
			if (!n->dec_slope[i]) return 0;
		}

		n->cat[i] = (double*) malloc0 ((size_t)cin * nf * sizeof (double));
		n->dec[i] = create_convt2d (cin, cout, nf,
			NNET_KT, NNET_KF, NNET_SF, NNET_PF, 0, w, b);

		want = n->nf[3 - i];
		if (getFout_convt2d (n->dec[i]) != want)
		{
			dprintf ("nnet: dec%d produces %d bins, expected %d\n",
				i + 1, getFout_convt2d (n->dec[i]), want);
			return 0;
		}
		n->d[i] = (double*) malloc0 ((size_t)cout * want * sizeof (double));
	}

	n->df = create_dfhead (n->nbins, n->order, n->floor_db, n->gmax_db);
	setSmooth_dfhead (n->df, n->sm_att, n->sm_rel, (double) NNET_FRAME_RATE);
	return 1;
}

NNET create_nnet (int nbins, int lookahead, double floor_db)
{
	return create_nnet_slot (0, nbins, lookahead, floor_db);
}

NNET create_nnet_slot (int slot, int nbins, int lookahead, double floor_db)
{
	NNET n = (NNET) malloc0 (sizeof (nnet));
	n->slot      = slot;
	n->nbins     = nbins;
	n->lookahead = lookahead;
	n->tau = NNET_TAU_DEFAULT;
	n->gmax_db = NNET_GMAX_DB;
	n->sm_att = 0.0;
	n->sm_rel = 0.0;
	n->floor_db  = floor_db;

	n->cnd = create_cond (nbins, 0.3, n->tau, 62.5, 1.0e-9);

	n->ready = nnet_build (n);
	if (!n->ready)
		dprintf ("nnet: no usable model at '%s' - passing audio through\n",
			nnet_model_path);
	else
		dprintf ("nnet: model loaded - ch %d/%d/%d/%d, hid %d, order %d, "
			"bins %d/%d/%d/%d/%d\n",
			n->c[0], n->c[1], n->c[2], n->c[3], n->hid, n->order,
			n->nf[0], n->nf[1], n->nf[2], n->nf[3], n->nf[4]);

	flush_nnet (n);
	return n;
}

void destroy_nnet (NNET n)
{
	int i;
	if (n->ready)
	{
		destroy_dfhead (n->df);
		for (i = 0; i < 4; i++)
		{
			destroy_convt2d (n->dec[i]);
			_aligned_free (n->d[i]);
			_aligned_free (n->cat[i]);
		}
		for (i = 0; i < 2; i++)
		{
			destroy_dprnn (n->dp[i]);
			_aligned_free (n->p[i]);
		}
		for (i = 0; i < 4; i++)
		{
			destroy_conv2d (n->enc[i]);
			_aligned_free (n->e[i]);
		}
	}
	if (n->mf) nnio_close (n->mf);
	destroy_cond (n->cnd);
	_aligned_free (n);
}

void flush_nnet (NNET n)
{
	int i;
	flush_cond (n->cnd);
	if (!n->ready) return;
	for (i = 0; i < 4; i++) flush_conv2d  (n->enc[i]);
	for (i = 0; i < 2; i++) flush_dprnn   (n->dp[i]);
	for (i = 0; i < 4; i++) flush_convt2d (n->dec[i]);
	flush_dfhead (n->df);
}

static void nnet_cat
	(
	double* dst,
	const double* a, int ca,
	const double* b, int cb,
	int nf
	)
{
	memcpy (dst,                   a, (size_t)ca * nf * sizeof (double));
	memcpy (dst + (size_t)ca * nf, b, (size_t)cb * nf * sizeof (double));
}

#ifdef NNET_PROFILE

double    nnp_sec[NNP_NSTAGE];
long long nnp_cnt[NNP_NSTAGE];

static const char* nnp_name[NNP_NSTAGE] = {
    "encoder 1", "encoder 2", "encoder 3", "encoder 4",
    "dual-path 1", "dual-path 2",
    "decoder 1", "decoder 2", "decoder 3", "decoder 4",
    "cond (normalizer)", "deep filter head", "TOTAL runCore"
};

double nnp_now (void)
{
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency (&f);
    QueryPerformanceCounter (&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

static long long nnp_frames = 0;
static int       nnp_done   = 0;

void reset_profile_nnet (void)
{
    int i;
    for (i = 0; i < NNP_NSTAGE; i++) { nnp_sec[i] = 0.0; nnp_cnt[i] = 0; }
    nnp_frames = 0;
    nnp_done = 0;
}

void report_profile_nnet (void)
{
    FILE* f;
    int i;
    double tot = nnp_sec[NNP_TOTAL];
    long long nf = nnp_cnt[NNP_TOTAL];

    if (nf == 0) { dprintf ("NNR profile: no frames measured\n"); return; }

    f = fopen (NNP_OUTFILE, "w");
    if (f == NULL)
    {
        dprintf ("NNR profile: could not open %s for writing\n", NNP_OUTFILE);
        return;
    }

#if defined(_WIN32) || defined(_WIN64)
    {
        char full[1024];
        if (_fullpath (full, NNP_OUTFILE, sizeof (full)) != NULL)
            dprintf ("NNR profile written to %s\n", full);
        else
            dprintf ("NNR profile written to %s\n", NNP_OUTFILE);
    }
#else
    dprintf ("NNR profile written to %s\n", NNP_OUTFILE);
#endif

    fprintf (f, "NNR inference profile\n");
    fprintf (f, "=====================\n\n");
    fprintf (f, "  %lld frames timed, after %d discarded for warm-up.\n",
             nf, NNP_WARMUP_FRAMES);
    fprintf (f, "  One frame is 16 ms of audio (hop 256 at 16000 Hz).\n\n");
    fprintf (f, "  %-20s %12s %10s %9s %9s\n",
             "stage", "total (ms)", "us/frame", "share", "core %");
    fprintf (f, "  %-20s %12s %10s %9s %9s\n",
             "--------------------", "------------", "----------",
             "---------", "---------");
    for (i = 0; i < NNP_TOTAL; i++)
    {
        double us;
        if (nnp_cnt[i] == 0) continue;
        us = 1e6 * nnp_sec[i] / (double)nf;
        fprintf (f, "  %-20s %12.1f %10.1f %8.1f%% %8.2f%%\n",
                 nnp_name[i], 1e3 * nnp_sec[i], us,
                 100.0 * nnp_sec[i] / (tot > 0.0 ? tot : 1.0),
                 100.0 * us / 16000.0);
    }
    fprintf (f, "  %-20s %12.1f %10.1f %8.1f%% %8.2f%%\n",
             nnp_name[NNP_TOTAL], 1e3 * tot, 1e6 * tot / (double)nf,
             100.0, 100.0 * (1e6 * tot / (double)nf) / 16000.0);
    fprintf (f,
        "\n  'core %%' is the share of ONE core this stage needs to keep up\n"
        "  in real time.  The TOTAL row is the number that decides whether a\n"
        "  model fits, and it is directly comparable between model sizes.\n"
        "\n  The stages sum to slightly less than the total; the difference\n"
        "  is the copies and concatenations between them.\n"
        "\n  MEASURE A RELEASE BUILD.  In Debug, MSVC inlines nothing, so the\n"
        "  small hot functions pay call overhead that Release removes - the\n"
        "  total is inflated and the shares between stages shift.\n");
    fclose (f);
}

#endif


void runCore_nnet (NNET n, const double* x)
{
	NNP_START (NNP_TOTAL);
	const double* y;
	int i, cout, nf;

	if (!n->ready) return;

	for (i = 0; i < 4; i++)
	{
		NNP_START (NNP_ENC0);
		y = run_conv2d (n->enc[i], (i == 0) ? x : n->e[i - 1]);
		memcpy (n->e[i], y, (size_t)n->c[i] * n->nf[i + 1] * sizeof (double));
		run_prelu (n->e[i], n->enc_slope[i], n->c[i], n->nf[i + 1]);
#ifdef NNET_PROFILE
		nnp_sec[NNP_ENC0 + i] += nnp_now() - _nnp_tNNP_ENC0;
		nnp_cnt[NNP_ENC0 + i] += 1;
#endif
	}

	for (i = 0; i < 2; i++)
	{
		NNP_START (NNP_DP0);
		y = run_dprnn (n->dp[i], (i == 0) ? n->e[3] : n->p[0]);
		memcpy (n->p[i], y, (size_t)n->c[3] * n->nf[4] * sizeof (double));
#ifdef NNET_PROFILE
		nnp_sec[NNP_DP0 + i] += nnp_now() - _nnp_tNNP_DP0;
		nnp_cnt[NNP_DP0 + i] += 1;
#endif
	}

	for (i = 0; i < 4; i++)
	{
		const double* up   = (i == 0) ? n->p[1] : n->d[i - 1];
		const double* skip = n->e[3 - i];
		int cup  = (i == 0) ? n->c[3] : n->c[3 - i];
		int cskp = n->c[3 - i];

		nf   = n->nf[4 - i];
		cout = (i == 3) ? 2 * n->order : n->c[2 - i];

		nnet_cat (n->cat[i], up, cup, skip, cskp, nf);

		NNP_START (NNP_DEC0);
		y = run_convt2d (n->dec[i], n->cat[i]);
		memcpy (n->d[i], y, (size_t)cout * n->nf[3 - i] * sizeof (double));
		if (i < 3)
			run_prelu (n->d[i], n->dec_slope[i], cout, n->nf[3 - i]);
#ifdef NNET_PROFILE
		nnp_sec[NNP_DEC0 + i] += nnp_now() - _nnp_tNNP_DEC0;
		nnp_cnt[NNP_DEC0 + i] += 1;
#endif
	}
#ifdef NNET_PROFILE
	if (!nnp_done)
	{
		nnp_frames += 1;
		if (nnp_frames == NNP_WARMUP_FRAMES)
		{
			int k;
			for (k = 0; k < NNP_NSTAGE; k++)
				{ nnp_sec[k] = 0.0; nnp_cnt[k] = 0; }
		}
		else if (nnp_frames > NNP_WARMUP_FRAMES)
		{
			nnp_sec[NNP_TOTAL] += nnp_now() - _nnp_tNNP_TOTAL;
			nnp_cnt[NNP_TOTAL] += 1;
			if (nnp_cnt[NNP_TOTAL] >= NNP_MEASURE_FRAMES)
			{
				nnp_done = 1;
				report_profile_nnet ();
			}
		}
	}
#endif
}

static void nnet_testmode (NNET n, const double* xre, const double* xim,
	double* yre, double* yim);

void run_nnet
	(
	NNET n,
	const double* xre,
	const double* xim,
	double* yre,
	double* yim
	)
{
	const double* cx;

	if (n->mode != NNET_MODE_NETWORK)
	{
		nnet_testmode (n, xre, xim, yre, yim);
		return;
	}

	if (!n->ready)
	{
		memcpy (yre, xre, n->nbins * sizeof (double));
		memcpy (yim, xim, n->nbins * sizeof (double));
		return;
	}

	NNP_START (NNP_COND);
	cx = run_cond (n->cnd, xre, xim);
	NNP_STOP (NNP_COND);

	runCore_nnet (n, cx);

	NNP_START (NNP_DFHEAD);
	run_dfhead (n->df, xre, xim, n->d[3], yre, yim);
	NNP_STOP (NNP_DFHEAD);
}

/********************************************************************************************************
*																										*
*										Properties / Diagnostics										*
*																										*
********************************************************************************************************/

static void nnet_testmode
	(
	NNET n,
	const double* xre,
	const double* xim,
	double* yre,
	double* yim
	)
{
	int k;

	switch (n->mode)
	{
	case NNET_MODE_LOWPASS:
		for (k = 0; k < n->nbins; k++)
		{
			yre[k] = (k < n->nbins / 2) ? xre[k] : 0.0;
			yim[k] = (k < n->nbins / 2) ? xim[k] : 0.0;
		}
		break;

	case NNET_MODE_IDENTITY:
	default:
		memcpy (yre, xre, n->nbins * sizeof (double));
		memcpy (yim, xim, n->nbins * sizeof (double));
		break;
	}
}

const double* getStage_nnet (NNET n, const char* name, int* nch, int* nfreq)
{
	static const char* names[NNET_NSTAGE] =
		{ "e1", "e2", "e3", "e4", "p1", "p2", "d1", "d2", "d3", "d4" };
	int i;

	if (!n->ready) return 0;

	for (i = 0; i < NNET_NSTAGE; i++)
		if (strcmp (name, names[i]) == 0) break;
	if (i == NNET_NSTAGE) return 0;

	if (i < 4)
	{
		*nch = n->c[i];
		*nfreq = n->nf[i + 1];
		return n->e[i];
	}
	if (i < 6)
	{
		*nch = n->c[3];
		*nfreq = n->nf[4];
		return n->p[i - 4];
	}
	i -= 6;
	*nch   = (i == 3) ? 2 * n->order : n->c[2 - i];
	*nfreq = n->nf[3 - i];
	return n->d[i];
}

void setMode_nnet (NNET n, int mode)
{
	n->mode = mode;
}

int getMode_nnet (NNET n)
{
	return n->mode;
}

void setSmooth_nnet (NNET n, double att_ms, double rel_ms)
{
	n->sm_att = att_ms;
	n->sm_rel = rel_ms;
	if (n->df)
		setSmooth_dfhead (n->df, att_ms, rel_ms,
			(double) NNET_FRAME_RATE);
}

void getSmooth_nnet (NNET n, double* att_ms, double* rel_ms)
{
	if (att_ms) *att_ms = n->sm_att;
	if (rel_ms) *rel_ms = n->sm_rel;
}

void setMaxGain_nnet (NNET n, double gmax_db)
{
	if (gmax_db < 0.0)  gmax_db = 0.0;
	if (gmax_db > 24.0) gmax_db = 24.0;
	n->gmax_db = gmax_db;
	if (n->ready) setGains_dfhead (n->df, n->floor_db, n->gmax_db);
}

double getMaxGain_nnet (NNET n)
{
	return n->gmax_db;
}

void setTau_nnet (NNET n, double tau)
{
	if (tau < 0.05) tau = 0.05;
	if (tau > 30.0) tau = 30.0;
	n->tau = tau;
	if (n->cnd) setTau_cond (n->cnd, tau);
}

double getTau_nnet (NNET n)
{
	return n->tau;
}

void setAlpha_nnet (NNET n, double alpha)
{
	setAlpha_dfhead (n->df, alpha);
}

double getAlpha_nnet (NNET n)
{
	return getAlpha_dfhead (n->df);
}

void setKnee_nnet (NNET n, double knee_db)
{
	setKnee_dfhead (n->df, knee_db);
}

double getKnee_nnet (NNET n)
{
	return getKnee_dfhead (n->df);
}

void setFloor_nnet (NNET n, double floor_db)
{
	n->floor_db = floor_db;
	if (n->ready) setGains_dfhead (n->df, floor_db, n->gmax_db);
}

int isReady_nnet (NNET n)
{
	return n->ready;
}

int ok_nnet (NNET n)
{
	return n != 0 && n->ready;
}

int getBins_nnet (NNET n)
{
	return n->nbins;
}

int getLookahead_nnet (NNET n)
{
	return n->lookahead;
}

PORT void
SetNNRModelPathSlot (int slot, const char* path)
{
	if (slot < 0 || slot >= NNET_NSLOTS)
	{
		dprintf ("nnet: model slot %d is out of range (0..%d)\n",
				 slot, NNET_NSLOTS - 1);
		return;
	}
	if (path == 0) path = "";
	strncpy (nnet_model_path[slot], path, sizeof (nnet_model_path[slot]) - 1);
	nnet_model_path[slot][sizeof (nnet_model_path[slot]) - 1] = '\0';
}

PORT void
SetNNRModelPath (const char* path)
{
	SetNNRModelPathSlot (0, path);
}
