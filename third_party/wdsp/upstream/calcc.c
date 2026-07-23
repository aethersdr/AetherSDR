/*  calcc.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2014, 2016, 2019, 2023, 2026 Warren Pratt, NR0V

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
#include "extrapolate.h"
#include "nurbs_spline.h"
#include "nurbs_fit.h"

#define SCHECK_PTS         40

static void init_collection (psCollection* collect)
{
	collect->bbtm [0] = 0.0050; collect->tpb [0] = 256;
	collect->bbtm [1] = 0.0625; collect->tpb [1] = 256;
	collect->bbtm [2] = 0.1250; collect->tpb [2] = 256;
	collect->bbtm [3] = 0.1875; collect->tpb [3] = 256;
	collect->bbtm [4] = 0.2500; collect->tpb [4] = 256;
	collect->bbtm [5] = 0.3125; collect->tpb [5] = 256;
	collect->bbtm [6] = 0.3750; collect->tpb [6] = 256;
	collect->bbtm [7] = 0.4375; collect->tpb [7] = 256;
	collect->bbtm [8] = 0.5000; collect->tpb [8] = 256;
	collect->bbtm [9] = 0.5625; collect->tpb [9] = 256;
	collect->bbtm[10] = 0.6250; collect->tpb[10] = 256;
	collect->bbtm[11] = 0.6875; collect->tpb[11] = 256;
	collect->bbtm[12] = 0.7500; collect->tpb[12] = 256;
	collect->bbtm[13] = 0.8125; collect->tpb[13] = 256;
	collect->bbtm[14] = 0.8750; collect->tpb[14] = 256;
	collect->bbtm[15] = 0.9375; collect->tpb[15] = 256;
	collect->bbtm[NBUCKS] = 1.0;
	int n = 0;
	for (int i = 0; i < NBUCKS; i++)
	{
		collect->bidx[i]  = n;
		collect->nidx[i]  = 0;
		collect->cpb[i]   = 0;
		collect->bfull[i] = 0;
		n += collect->tpb[i];
	}
	collect->nsamps = n;
	collect->smps = (psSample*)malloc0 (n * sizeof(psSample));
	collect->nfull = 0;
}

static void delete_collection (psCollection* collect)
{
	_aligned_free(collect->smps);
}

static int find_range_index (const double bounds[], int n, double val)
{
	if (n < 2) return -1;
	int low = 0;
	int high = n - 1;
	if (val < bounds[low]) return -1;
	if (val >= bounds[high]) return n - 1;
	int mid = 0;
	while (high - low > 1)
	{
		mid = low + (high - low) / 2;
		if (val >= bounds[mid])
			low = mid;
		else
			high = mid;
	}
	return low;
}

static void putSample (double* tx, double* rx, double hw_scale, psCollection* Collect)
{
	double env_tx = sqrt(tx[0] * tx[0] + tx[1] * tx[1]);
	double env_rx = sqrt(rx[0] * rx[0] + rx[1] * rx[1]);
	if (env_tx < 1.0e-30 || env_rx < 1.0e-30) return;
	int buck = find_range_index(Collect->bbtm, NBUCKS + 1, env_tx * hw_scale);
	if (buck < 0) return;
	if (buck >= NBUCKS)
	{
		if (ACCEPT_OVERRANGE)  buck = NBUCKS - 1;
		else return;
	}
	int index_to_fill = Collect->bidx[buck] + Collect->nidx[buck];
	Collect->nidx[buck] = (Collect->nidx[buck] + 1) % Collect->tpb[buck];
	if (Collect->cpb[buck] == (Collect->tpb[buck] - 1)) ++(Collect->nfull);
	if (Collect->cpb[buck] < Collect->tpb[buck]) ++(Collect->cpb[buck]);
	if (Collect->cpb[buck] == Collect->tpb[buck]) Collect->bfull[buck] = 1;
	Collect->smps[index_to_fill].tx.I = tx[0];
	Collect->smps[index_to_fill].tx.Q = tx[1];
	Collect->smps[index_to_fill].rx.I = rx[0];
	Collect->smps[index_to_fill].rx.Q = rx[1];
	Collect->smps[index_to_fill].envTX = env_tx;
	Collect->smps[index_to_fill].envRX = env_rx;
}

static int sampleCheckAndUpdate (psCollection* Collect)
{
	int rval = 0;
	if (Collect->nfull == NBUCKS)
	{
		rval = 1;
		Collect->nfull = 0;
		for (int i = 0; i < NBUCKS; i++)
		{
			Collect->nidx[i] = 0;
			Collect->cpb[i] = 0;
			Collect->bfull[i] = 0;
		}
	}
	return rval;
}

static void sampleCollectClear(psCollection* Collect)
{
	Collect->nfull = 0;
	for (int i = 0; i < NBUCKS; i++)
	{
		Collect->nidx[i] = 0;
		Collect->cpb[i] = 0;
		Collect->bfull[i] = 0;
	}
}

static void size_calcc (CALCC a)
{
	a->nsamps = a->PS_Colct.nsamps;

	a->env_TX = (double*)malloc0(a->nsamps * sizeof(double));
	a->env_RX = (double*)malloc0(a->nsamps * sizeof(double));
	a->x = (double*)malloc0(a->nsamps * sizeof(double));
	a->ym = (double*)malloc0(a->nsamps * sizeof(double));
	a->yc = (double*)malloc0(a->nsamps * sizeof(double));
	a->ys = (double*)malloc0(a->nsamps * sizeof(double));

	a->m_config = (NF_Config*)malloc0(sizeof(NF_Config));
	a->c_config = (NF_Config*)malloc0(sizeof(NF_Config));
	a->s_config = (NF_Config*)malloc0(sizeof(NF_Config));

	a->m_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));
	a->c_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));
	a->s_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));

	a->eq_enable = 1;
	a->eq_nbins = 100;
	a->eq_min_pts = 60;
	a->eq_min_cnt = 3;
	a->eq_robust_x = 0.0;// 0.20;  // revert to 'mean' mode for now.
	a->eq_rxmin = 0.005;
	a->eq_n = 0;
	a->m_eqd = (NF_Point2*)malloc0(a->eq_nbins * sizeof(NF_Point2));
	a->c_eqd = (NF_Point2*)malloc0(a->eq_nbins * sizeof(NF_Point2));
	a->s_eqd = (NF_Point2*)malloc0(a->eq_nbins * sizeof(NF_Point2));

	a->m_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));
	a->c_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));
	a->s_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));

	a->disp.cpts   = 512;
	a->disp.nsamps = 0;
	a->disp.x  = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.ym = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.yc = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.ys = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.xm_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.ym_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.xc_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.yc_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.xs_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.ys_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.xa_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));
	a->disp.ya_cor = (double *) malloc0 (a->disp.cpts * sizeof (double));

	a->ema_alpha    = 0.30;
	a->ema_alpha_lo = 0.30;
	a->ema_x_bnd    = 0.20;
	a->pin_alpha    = 0.10;

	a->m_extend_left = 1;
	a->c_extend_left = 0;
	a->s_extend_left = 0;

	a->extend_bound_frac = 0.05;
	a->extend_x_target   = 0.0;

	a->dcb_enabled     = 0;
	a->dcb_thresh      = 0.05;
	a->dcb_cap         = 0.25;
	a->dcb_floor       = 0.03;
	a->dcb_nbins       = 24;
	a->dcb_confirm     = 2;
	a->dcb_min_per_bin = 20;
	a->dcb_alpha       = 0.30;

	a->m_anchor_ema   = 0.04;
	a->c_anchor_ema   = 0.04;
	a->s_anchor_ema   = 0.04;
	a->m_anchor_valid = 0;
	a->c_anchor_valid = 0;
	a->s_anchor_valid = 0;

	curve_ema_init2(&a->m_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd,  0.1, 2.0);
	curve_ema_init2(&a->c_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd, -1.1, 1.1);
	curve_ema_init2(&a->s_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd, -1.1, 1.1);

	const int CTRL_MAX = 32;
	a->m_fold_prev      = 0;
	a->m_ctrl_n         = 0;
	a->m_ctrl_ema_x     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->m_ctrl_ema_y     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->m_ctrl_ema_valid = 0;
	a->c_ctrl_n         = 0;
	a->c_ctrl_ema_x     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->c_ctrl_ema_y     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->c_ctrl_ema_valid = 0;
	a->s_ctrl_n         = 0;
	a->s_ctrl_ema_x     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->s_ctrl_ema_y     = (double*)malloc0(CTRL_MAX * sizeof(double));
	a->s_ctrl_ema_valid = 0;

	a->c_y_pin_ema   = 1.0;
	a->c_y_pin_valid = 0;
	a->c_pin_cycle   = 0;
	a->s_y_pin_ema   = 0.0;
	a->s_y_pin_valid = 0;
	a->s_pin_cycle   = 0;

	a->m_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->c_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->s_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->scheck_valid = 0;
}

static void desize_calcc (CALCC a)
{
	_aligned_free (a->disp.ya_cor);
	_aligned_free (a->disp.xa_cor);
	_aligned_free (a->disp.ys_cor);
	_aligned_free (a->disp.xs_cor);
	_aligned_free (a->disp.yc_cor);
	_aligned_free (a->disp.xc_cor);
	_aligned_free (a->disp.ym_cor);
	_aligned_free (a->disp.xm_cor);

	_aligned_free (a->disp.ys);
	_aligned_free (a->disp.yc);
	_aligned_free (a->disp.ym);
	_aligned_free (a->disp.x);

	_aligned_free(a->m_nfres);
	_aligned_free(a->c_nfres);
	_aligned_free(a->s_nfres);

	_aligned_free(a->s_eqd);
	_aligned_free(a->c_eqd);
	_aligned_free(a->m_eqd);

	_aligned_free(a->s_data);
	_aligned_free(a->c_data);
	_aligned_free(a->m_data);

	_aligned_free(a->m_ctrl_ema_x); a->m_ctrl_ema_x = NULL;
	_aligned_free(a->m_ctrl_ema_y); a->m_ctrl_ema_y = NULL;
	_aligned_free(a->c_ctrl_ema_x); a->c_ctrl_ema_x = NULL;
	_aligned_free(a->c_ctrl_ema_y); a->c_ctrl_ema_y = NULL;
	_aligned_free(a->s_ctrl_ema_x); a->s_ctrl_ema_x = NULL;
	_aligned_free(a->s_ctrl_ema_y); a->s_ctrl_ema_y = NULL;

	_aligned_free(a->m_prev_sol); a->m_prev_sol = NULL;
	_aligned_free(a->c_prev_sol); a->c_prev_sol = NULL;
	_aligned_free(a->s_prev_sol); a->s_prev_sol = NULL;

	_aligned_free(a->s_config);
	_aligned_free(a->c_config);
	_aligned_free(a->m_config);

	_aligned_free(a->x);
	_aligned_free(a->ym);
	_aligned_free(a->yc);
	_aligned_free(a->ys);
	_aligned_free(a->env_TX);
	_aligned_free(a->env_RX);
}

CALCC create_calcc (int channel, int runcal, int size, int rate, double hw_scale,
	double moxdelay, double loopdelay, int mox)
{
	CALCC a = (CALCC) malloc0 (sizeof (calcc));
	a->channel = channel;
	a->runcal = runcal;
	a->size = size;
	a->rate = rate;
	a->hw_scale = hw_scale;
	a->ctrl.moxdelay = moxdelay;
	a->ctrl.loopdelay = loopdelay;
	a->mox = mox;

	init_collection(&a->PS_Colct);

	a->info  = (int *) malloc0 (16 * sizeof (int));
	a->binfo = (int *) malloc0 (16 * sizeof (int));

	a->ctrl.state = 0;
	a->ctrl.reset = 0;
	a->ctrl.automode = 0;
	a->ctrl.mancal = 0;
	a->ctrl.turnon = 0;
	a->ctrl.moxsamps = (int)(a->rate * a->ctrl.moxdelay);
	a->ctrl.moxcount = 0;
	a->ctrl.count = 0;
	a->ctrl.calcinprogress = 0;
	a->ctrl.calcdone = 0;
	a->ctrl.waitsamps = (int)(a->rate * a->ctrl.loopdelay);
	a->ctrl.waitcount = 0;
	a->ctrl.running = 0;
	a->ctrl.current_state = 0;
	InitializeCriticalSectionAndSpinCount (&txa[a->channel].calcc.cs_update, 2500);
	a->rxdelay = create_delay (
		1,
		0,
		0,
		0,
		a->rate,
		20.0e-09,
		0.0);
	a->txdelay = create_delay (
		1,
		0,
		0,
		0,
		a->rate,
		20.0e-09,
		0.0);

	InitializeCriticalSectionAndSpinCount (&a->disp.cs_disp, 2500);

	size_calcc (a);

	for (int i = 0; i < 5; i++)
	{
		a->SemsPSCorr[i] = CreateSemaphoreW(0, 0, 1, 0);
	}
	a->hCorrChangeExited = CreateEvent(NULL, FALSE, FALSE, NULL);
	_beginthread(doPSCorrChange, 0, (void*)a);
	return a;
}

void destroy_calcc (CALCC a)
{
	IQC b = txa[a->channel].iqc.p;

	ns_free(a->util.m_spline_restore); a->util.m_spline_restore = NULL;
	ns_free(a->util.c_spline_restore); a->util.c_spline_restore = NULL;
	ns_free(a->util.s_spline_restore); a->util.s_spline_restore = NULL;
	ns_free(a->util.m_spline_save);    a->util.m_spline_save = NULL;
	ns_free(a->util.c_spline_save);    a->util.c_spline_save = NULL;
	ns_free(a->util.s_spline_save);    a->util.s_spline_save = NULL;

	for (int i = 0; i < 4; i++)
		while (WaitForSingleObject(a->SemsPSCorr[i], 0) == WAIT_OBJECT_0);
	InterlockedBitTestAndReset(&b->busy, 0);
	ReleaseSemaphore(a->SemsPSCorr[4], 1, 0);
	WaitForSingleObject(a->hCorrChangeExited, 500);
	CloseHandle(a->hCorrChangeExited);

	ns_free(a->m_spline); a->m_spline = NULL;
	ns_free(a->c_spline); a->c_spline = NULL;
	ns_free(a->s_spline); a->s_spline = NULL;

	desize_calcc (a);
	DeleteCriticalSection (&a->disp.cs_disp);
	destroy_delay (a->txdelay);
	destroy_delay (a->rxdelay);
	DeleteCriticalSection (&txa[a->channel].calcc.cs_update);
	_aligned_free (a->binfo);
	_aligned_free (a->info);

	delete_collection (&a->PS_Colct);

	_aligned_free (a);
}

void flush_calcc (CALCC a)
{
	flush_delay (a->rxdelay);
	flush_delay (a->txdelay);
}

#define EXTREMA_CHECK     0.05

static int count_extrema(const NS_Spline *s, double x_lo, double x_hi,
                          int n_steps)
{
	if (!s || n_steps < 3) return 0;
	const double min_prom = EXTREMA_CHECK;
	double* ys = (double*)malloc(n_steps * sizeof(double));
	if (!ys) return 0;
	double prev_y = ns_eval_near_clamped(s, x_lo, 1.0);
	for (int i = 0; i < n_steps; i++)
	{
		double x = x_lo + (x_hi - x_lo) * i / (n_steps - 1);
		ys[i]    = ns_eval_near_clamped(s, x, prev_y);
		prev_y   = ys[i];
	}
	int extrema = 0;
	int    dir = 0;
	double ext = ys[0];

	for (int i = 1; i < n_steps; i++)
	{
		double y = ys[i];
		if (dir == 0)
	{
			if (y > ys[0] + min_prom) dir = 1;
			else if (y < ys[0] - min_prom) dir = -1;
			if (dir >= 0 && y > ext) ext = y;
			if (dir <= 0 && y < ext) ext = y;
			continue;
		}
		if (dir > 0)
		{
			if (y > ext) ext = y;
			else if (ext - y >= min_prom)
			{
				extrema++;
				dir = -1; ext = y;
			}
		}
		else
		{
			if (y < ext) ext = y;
			else if (y - ext >= min_prom)
			{
				extrema++;
				dir = 1; ext = y;
			}
		}
	}
	free(ys);
	return extrema;
}

static int dcb_cmp_double(const void* pa, const void* pb)
{
	double a = *(const double*)pa, b = *(const double*)pb;
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static double detect_clean_boundary(const double* x, const double* y,
                                    const double* denom, int n,
                                    double x_lo, double x_hi, int nbins,
                                    double thresh, int confirm,
                                    double floor_x, double cap_x,
                                    int min_per_bin)
{
	if (n < min_per_bin) return cap_x;

	double  bw   = (x_hi - x_lo) / (double)nbins;
	double* tmp  = (double*)malloc(n * sizeof(double));
	double* tmpd = (double*)malloc(n * sizeof(double));
	if (!tmp || !tmpd) { free(tmp); free(tmpd); return cap_x; }

	int run = 0;
	double result = cap_x;
	int found = 0;

	for (int b = 0; b < nbins && !found; b++)
	{
		double blo = x_lo + bw * b;
		double bhi = blo + bw;
		double bcenter = 0.5 * (blo + bhi);

		int m = 0;
		for (int i = 0; i < n; i++)
		{
			if (x[i] >= blo && x[i] < bhi)
			{
				tmp[m]  = y[i];
				tmpd[m] = denom ? denom[i] : y[i];
				m++;
			}
		}
		if (m < min_per_bin) { run = 0; continue; }

		qsort(tmp, m, sizeof(double), dcb_cmp_double);
		double q1 = tmp[(int)(0.25 * (m - 1))];
		double q3 = tmp[(int)(0.75 * (m - 1))];
		double iqr = q3 - q1;

		qsort(tmpd, m, sizeof(double), dcb_cmp_double);
		double dmed = tmpd[m / 2];
		if (fabs(dmed) < 1e-6) { run = 0; continue; }

		double rel = iqr / fabs(dmed);

		if (rel < thresh)
		{
			run++;
			if (run >= confirm)
			{
				double xb = bcenter - bw * (confirm - 1);
				result = xb;
				found = 1;
			}
		}
		else
		{
			run = 0;
		}
	}

	free(tmp);
	free(tmpd);

	if (result < floor_x) result = floor_x;
	if (result > cap_x)   result = cap_x;
	return result;
}

static int eq_dcmp(const void* p1, const void* p2)
{
	double d1 = *(const double*)p1, d2 = *(const double*)p2;
	return (d1 > d2) - (d1 < d2);
}

static double eq_median(double* v, int n)
{
	qsort(v, n, sizeof(double), eq_dcmp);
	if (n & 1) return v[n >> 1];
	return 0.5 * (v[(n >> 1) - 1] + v[n >> 1]);
}

static int equalize_density(CALCC a)
{
	const int nb = a->eq_nbins;
	int* cnt = (int*)malloc0(nb * sizeof(int));
	double* sx = (double*)malloc0(nb * sizeof(double));
	double* sm = (double*)malloc0(nb * sizeof(double));
	double* sc = (double*)malloc0(nb * sizeof(double));
	double* ss = (double*)malloc0(nb * sizeof(double));
	for (int i = 0; i < a->nsamps; i++)
	{
		if (a->env_RX[i] < a->eq_rxmin) continue;
		int b = (int)(a->x[i] * nb);
		if (b < 0)      b = 0;
		if (b > nb - 1) b = nb - 1;
		cnt[b]++;
		sx[b] += a->x[i];
		sm[b] += a->ym[i];
		sc[b] += a->yc[i];
		ss[b] += a->ys[i];
	}
	const int nb_lo = (int)(a->eq_robust_x * (double)nb + 0.5);
	double* med_m = NULL;
	double* med_c = NULL;
	double* med_s = NULL;
	if (nb_lo > 0)
	{
		int lo_total = 0;
		for (int b = 0; b < nb_lo; b++) lo_total += cnt[b];
		if (lo_total > 0)
		{
			int*    offs = (int*)   malloc0((nb_lo + 1) * sizeof(int));
			int*    fill = (int*)   malloc0( nb_lo      * sizeof(int));
			double* lm   = (double*)malloc0(lo_total * sizeof(double));
			double* lc   = (double*)malloc0(lo_total * sizeof(double));
			double* ls   = (double*)malloc0(lo_total * sizeof(double));
			med_m = (double*)malloc0(nb_lo * sizeof(double));
			med_c = (double*)malloc0(nb_lo * sizeof(double));
			med_s = (double*)malloc0(nb_lo * sizeof(double));
			for (int b = 0; b < nb_lo; b++) offs[b + 1] = offs[b] + cnt[b];
			for (int i = 0; i < a->nsamps; i++)
			{
				if (a->env_RX[i] < a->eq_rxmin) continue;
				int b = (int)(a->x[i] * nb);
				if (b < 0)      b = 0;
				if (b > nb - 1) b = nb - 1;
				if (b < nb_lo)
				{
					int idx = offs[b] + fill[b]++;
					lm[idx] = a->ym[i];
					lc[idx] = a->yc[i];
					ls[idx] = a->ys[i];
				}
			}
			for (int b = 0; b < nb_lo; b++)
			{
				if (cnt[b] >= 3)
				{
					med_m[b] = eq_median(lm + offs[b], cnt[b]);
					med_c[b] = eq_median(lc + offs[b], cnt[b]);
					med_s[b] = eq_median(ls + offs[b], cnt[b]);
				}
			}
			_aligned_free(ls); _aligned_free(lc); _aligned_free(lm);
			_aligned_free(fill); _aligned_free(offs);
		}
	}

	int n = 0;
	for (int b = 0; b < nb; b++)
	{
		if (cnt[b] == 0) continue;
		if (cnt[b] < a->eq_min_cnt) continue;
		double inv = 1.0 / (double)cnt[b];
		a->m_eqd[n].x = a->c_eqd[n].x = a->s_eqd[n].x = sx[b] * inv;
		a->m_eqd[n].y = sm[b] * inv;
		a->c_eqd[n].y = sc[b] * inv;
		a->s_eqd[n].y = ss[b] * inv;
		if (b < nb_lo && cnt[b] >= 3 && med_m != NULL)
		{
			a->m_eqd[n].y = med_m[b];
			a->c_eqd[n].y = med_c[b];
			a->s_eqd[n].y = med_s[b];
		}
		n++;
	}
	/*dprintf("eq: %d samples -> %d bin points; top-bin count = %d\n",
		a->nsamps, n, cnt[nb - 1]);*/
	if (med_s != NULL) _aligned_free(med_s);
	if (med_c != NULL) _aligned_free(med_c);
	if (med_m != NULL) _aligned_free(med_m);
	_aligned_free(ss); _aligned_free(sc); _aligned_free(sm);
	_aligned_free(sx); _aligned_free(cnt);
	return n;
}

static void calc (CALCC a)
{
	a->binfo[7]++;

	a->m_nurb = NULL;
	a->c_nurb = NULL;
	a->s_nurb = NULL;

	a->ctrl.env_maxtx = 0.0;
	for (int i = 0; i < a->nsamps; i++)
	{
		a->env_TX[i] = a->PS_Colct.smps[i].envTX;
		a->env_RX[i] = a->PS_Colct.smps[i].envRX;
		if (a->env_TX[i] > a->ctrl.env_maxtx) a->ctrl.env_maxtx = a->env_TX[i];
	}

	a->binfo[0] = 0b0000;

	double* norm_env_TX = (double*)malloc0(a->nsamps * sizeof(double));
	for (int k = 0; k < a->nsamps; k++)
		norm_env_TX[k] = a->hw_scale * a->env_TX[k];
	ExtrapolationResult Extrapolate_Res = extrapolate_y_at_1(norm_env_TX, a->env_RX, a->nsamps);
	_aligned_free(norm_env_TX);
	a->rx_scale = 1.0 / Extrapolate_Res.y_at_1;
	if (Extrapolate_Res.confidence)
	{
		a->binfo[0] |= 0b0001;
		goto cleanup;
	}

	a->binfo[4] = (int)(256.0 * (a->hw_scale / a->rx_scale));

	for (int i = 0; i < a->nsamps; i++)
	{
		const double slope = 0.001;
		double rx_c = a->env_RX[i];
		double max_rx = (1.0 - slope + slope * a->hw_scale * a->env_TX[i]) / a->rx_scale;
		if (rx_c > max_rx) rx_c = max_rx;
		a->x[i]  = a->rx_scale * rx_c;
		a->ym[i] = (a->hw_scale * a->env_TX[i]) / (a->rx_scale * rx_c);
		double norm = a->env_TX[i] * a->env_RX[i];
		a->yc[i] = (+ a->PS_Colct.smps[i].tx.I * a->PS_Colct.smps[i].rx.I
			        + a->PS_Colct.smps[i].tx.Q * a->PS_Colct.smps[i].rx.Q) / norm;
		a->ys[i] = (- a->PS_Colct.smps[i].tx.I * a->PS_Colct.smps[i].rx.Q
			        + a->PS_Colct.smps[i].tx.Q * a->PS_Colct.smps[i].rx.I) / norm;
	}

	if (a->dcb_enabled)
	{
		double* phasor_mag = (double*)malloc0(a->nsamps * sizeof(double));
		for (int i = 0; i < a->nsamps; i++)
			phasor_mag[i] = sqrt(a->yc[i]*a->yc[i] + a->ys[i]*a->ys[i]);

		double m_anc = detect_clean_boundary(a->x, a->ym, NULL, a->nsamps,
		                   0.0, a->dcb_cap + 0.05, a->dcb_nbins,
		                   a->dcb_thresh, a->dcb_confirm,
		                   a->dcb_floor, a->dcb_cap, a->dcb_min_per_bin);
		double c_anc = detect_clean_boundary(a->x, a->yc, phasor_mag, a->nsamps,
		                   0.0, a->dcb_cap + 0.05, a->dcb_nbins,
		                   a->dcb_thresh, a->dcb_confirm,
		                   a->dcb_floor, a->dcb_cap, a->dcb_min_per_bin);
		double s_anc = detect_clean_boundary(a->x, a->ys, phasor_mag, a->nsamps,
		                   0.0, a->dcb_cap + 0.05, a->dcb_nbins,
		                   a->dcb_thresh, a->dcb_confirm,
		                   a->dcb_floor, a->dcb_cap, a->dcb_min_per_bin);

		_aligned_free(phasor_mag);

		if (!a->m_anchor_valid) { a->m_anchor_ema = m_anc; a->m_anchor_valid = 1; }
		else a->m_anchor_ema = a->dcb_alpha * m_anc + (1.0 - a->dcb_alpha) * a->m_anchor_ema;
		if (!a->c_anchor_valid) { a->c_anchor_ema = c_anc; a->c_anchor_valid = 1; }
		else a->c_anchor_ema = a->dcb_alpha * c_anc + (1.0 - a->dcb_alpha) * a->c_anchor_ema;
		if (!a->s_anchor_valid) { a->s_anchor_ema = s_anc; a->s_anchor_valid = 1; }
		else a->s_anchor_ema = a->dcb_alpha * s_anc + (1.0 - a->dcb_alpha) * a->s_anchor_ema;

	}
	else
	{

		a->m_anchor_ema = a->dcb_floor; a->m_anchor_valid = 0;
		a->c_anchor_ema = a->dcb_floor; a->c_anchor_valid = 0;
		a->s_anchor_ema = a->dcb_floor; a->s_anchor_valid = 0;
	}

#define _DEBUG_FOLD_SILENTLY 1

	a->binfo[1] = 0x0000;

	int eq_used = 0;
	if (a->eq_enable)
	{
		a->eq_n = equalize_density(a);
		if (a->eq_n >= a->eq_min_pts) eq_used = 1;
	}

	for (int k = 0; k < a->nsamps; k++)
	{
		a->m_data[k].x = a->x[k];
		a->m_data[k].y = a->ym[k];
	}

	nf_default_config(a->m_config);
	a->m_config->ordering_mode       = NF_ORDER_BY_X;
	a->m_config->n_ctrl              = 20;
	a->m_config->adaptive_iters      = 0;
	a->m_config->outlier_sigma       = 2.5;
	a->m_config->local_outlier_iters = 0;
	a->m_config->local_outlier_bands = 20;
	a->m_config->x_weight_x0         = 0.15;
	a->m_config->x_weight_min        = 0.04;
	a->m_config->pre_filter_y_max    = 1.8;
	a->m_config->pre_filter_x_min    = 0.04;
	a->m_config->fold_detect         = 1;
	a->m_config->y_min               = 0.0;
	a->m_config->y_max               = 0.0;
	a->m_config->pin_end             = 1;
	a->m_config->end_pt              = (NF_Point2){ 1.0, 1.0 };
	a->m_config->pin_end_horiz       = a->m_fold_prev;
	a->m_config->pin_end_flat        = a->m_fold_prev;
	a->m_config->pin_end_flat2       = a->m_fold_prev;
	a->m_config->pin_start = 0;
	a->m_ctrl_n = a->m_config->n_ctrl;

	if (eq_used) a->m_nurb = nf_fit(a->m_eqd, a->eq_n, a->m_config, a->m_nfres);
	else         a->m_nurb = nf_fit(a->m_data, a->nsamps, a->m_config, a->m_nfres);

	if (a->m_nurb == NULL)
	{
		a->binfo[1] |= 0b0001;
		goto cleanup;
	}
	if (a->m_nfres->quality & NF_FIT_BAD)
	{
		a->binfo[1] |= 0b0010;
		goto cleanup;
	}

	a->m_fold_prev = a->m_nfres->fold_detected ? 1 : 0;
	{
		const double alpha = 0.10;
		int nc = a->m_nurb->n_ctrl;
		if (!a->m_ctrl_ema_valid)
		{
			const double COLD_START_LEFT_MAX = 1.8;
			double left_val = ns_eval_near_clamped(a->m_spline, 0.04,
			                      a->m_calavg.ys[0]);
			if (left_val > COLD_START_LEFT_MAX)
			{
				a->binfo[1] |= 0b00100000;
				goto cleanup;
			}
			for (int k = 0; k < nc; k++)
			{
				a->m_ctrl_ema_x[k] = a->m_nurb->ctrl_wx[k];
				a->m_ctrl_ema_y[k] = a->m_nurb->ctrl_wy[k];
			}
			a->m_ctrl_n         = nc;
			a->m_ctrl_ema_valid = 1;
		}
		else
		{
			for (int k = 0; k < nc; k++)
			{
				a->m_ctrl_ema_x[k] = alpha * a->m_nurb->ctrl_wx[k]
				                   + (1.0 - alpha) * a->m_ctrl_ema_x[k];
				a->m_ctrl_ema_y[k] = alpha * a->m_nurb->ctrl_wy[k]
				                   + (1.0 - alpha) * a->m_ctrl_ema_y[k];
			}
		}
		for (int k = 0; k < nc; k++)
		{
			a->m_nurb->ctrl_wx[k] = a->m_ctrl_ema_x[k];
			a->m_nurb->ctrl_wy[k] = a->m_ctrl_ema_y[k];
		}
	}

	a->m_spline = ns_build(a->m_nurb, a->m_nfres, 0);
	if (a->m_spline == NULL)
	{
		a->binfo[1] |= 0b0100;
		goto cleanup;
	}
	double m_max_err, m_rms_err;
	ns_accuracy_check(a->m_spline, a->m_nurb, 1000, &m_max_err, &m_rms_err);
	double m_noise_est = fmax(a->m_nfres->rms, a->m_nfres->cv_score);
	if (m_max_err > m_noise_est * 50.0)
	{
		a->binfo[1] |= 0b1000;

		ns_free(a->m_spline);
		a->m_spline = NULL;
		goto cleanup;
	}

	if (a->m_extend_left)
		ns_extend_left(a->m_spline, a->extend_x_target, a->m_anchor_ema,
		               a->extend_bound_frac, 0.1, 2.0);

	int m_extrema = count_extrema(a->m_spline, 0.05, 0.85, 100);
	if (m_extrema <= 2)
	{
		curve_ema_update(&a->m_calavg, a->m_spline);
	} else
	{
		a->binfo[1] |= 0b00010000;
		goto cleanup;
	}
	a->m_prev_y = a->m_calavg.ys[0];

	a->binfo[2] = 0x0000;
	for (int k = 0; k < a->nsamps; k++)
	{
		a->c_data[k].x = a->x[k];
		a->c_data[k].y = a->yc[k];
	}
	{
		ExtrapolationResult pin_res = extrapolate_y_at_0(
			a->x, a->yc, a->nsamps,
			0.001,
			0.15);
		double y_pin_raw = pin_res.y_at_1;
		if (y_pin_raw < -1.1) y_pin_raw = -1.1;
		if (y_pin_raw >  1.1) y_pin_raw =  1.1;
		if (!a->c_y_pin_valid)
		{
			a->c_y_pin_ema   = y_pin_raw;
			a->c_y_pin_valid = 1;
			a->c_pin_cycle   = 1;
		} else
		{
			const int    PIN_WARMUP_CYCLES = 5;
			const double PIN_WARMUP_ALPHA  = 0.40;
			double eff_alpha;
			if (a->c_pin_cycle <= PIN_WARMUP_CYCLES)
				eff_alpha = PIN_WARMUP_ALPHA;
			else
				eff_alpha = (pin_res.confidence == EXTRAP_CONFIDENT)
					? a->pin_alpha : a->pin_alpha * 0.5;
			a->c_y_pin_ema = eff_alpha * y_pin_raw
				+ (1.0 - eff_alpha) * a->c_y_pin_ema;
			if (a->c_pin_cycle <= PIN_WARMUP_CYCLES) a->c_pin_cycle++;
		}
	}

	nf_default_config(a->c_config);
	a->c_config->ordering_mode       = NF_ORDER_BY_X;
	a->c_config->n_ctrl              = 20;
	a->c_config->adaptive_iters      = 0;
	a->c_config->outlier_sigma       = 2.5;
	a->c_config->local_outlier_iters = 0;
	a->c_config->y_min               = -1.05;
	a->c_config->y_max               = +1.05;
	a->c_config->fold_detect         = 1;
	a->c_config->pre_filter_x_min    = 0.02;
	a->c_config->pin_start           = 1;
	a->c_config->start_pt            = (NF_Point2){ 0.0, a->c_y_pin_ema };
	a->c_config->pin_end             = 0;
	a->c_ctrl_n = a->c_config->n_ctrl;

	if (eq_used) a->c_nurb = nf_fit(a->c_eqd, a->eq_n, a->c_config, a->c_nfres);
	else         a->c_nurb = nf_fit(a->c_data, a->nsamps, a->c_config, a->c_nfres);

	if (a->c_nurb == NULL)
	{
		a->binfo[2] |= 0b0001;
		goto cleanup;
	}
	if (a->c_nfres->quality & NF_FIT_BAD)
	{
		a->binfo[2] |= 0b0010;
		goto cleanup;
	}
	{
		const double alpha = 0.10;
		int nc = a->c_nurb->n_ctrl;
		if (!a->c_ctrl_ema_valid)
		{
			const double COLD_START_COS_MAX = 1.1;
			double c_left_val = ns_eval_near_clamped(a->c_spline, 0.04,
			                        a->c_calavg.ys[0]);
			if (fabs(c_left_val) > COLD_START_COS_MAX)
			{
				a->binfo[2] |= 0b00100000;
				goto cleanup;
			}
			for (int k = 0; k < nc; k++)
			{
				a->c_ctrl_ema_x[k] = a->c_nurb->ctrl_wx[k];
				a->c_ctrl_ema_y[k] = a->c_nurb->ctrl_wy[k];
			}
			a->c_ctrl_n         = nc;
			a->c_ctrl_ema_valid = 1;
		} else
		{
			for (int k = 0; k < nc; k++)
			{
				a->c_ctrl_ema_x[k] = alpha * a->c_nurb->ctrl_wx[k]
				                   + (1.0 - alpha) * a->c_ctrl_ema_x[k];
				a->c_ctrl_ema_y[k] = alpha * a->c_nurb->ctrl_wy[k]
				                   + (1.0 - alpha) * a->c_ctrl_ema_y[k];
			}
		}
		for (int k = 0; k < nc; k++)
		{
			a->c_nurb->ctrl_wx[k] = a->c_ctrl_ema_x[k];
			a->c_nurb->ctrl_wy[k] = a->c_ctrl_ema_y[k];
		}
	}

	a->c_spline = ns_build(a->c_nurb, a->c_nfres, 0);
	if (a->c_spline == NULL)
	{
		a->binfo[2] |= 0b0100;
		goto cleanup;
	}
	double c_max_err, c_rms_err;
	ns_accuracy_check(a->c_spline, a->c_nurb, 1000, &c_max_err, &c_rms_err);
	double c_noise_est = fmax(a->c_nfres->rms, a->c_nfres->cv_score);
	if (c_max_err > c_noise_est * 50.0)
	{
		a->binfo[2] |= 0b1000;

		ns_free(a->c_spline);
		a->c_spline = NULL;
		goto cleanup;
	}
	if (a->c_extend_left)
		ns_extend_left(a->c_spline, a->extend_x_target, a->c_anchor_ema,
		               a->extend_bound_frac, -1.1, 1.1);
	curve_ema_update(&a->c_calavg, a->c_spline);
	a->c_prev_y = a->c_calavg.ys[0];

	a->binfo[3] = 0x0000;

	for (int k = 0; k < a->nsamps; k++)
	{
		a->s_data[k].x = a->x[k];
		a->s_data[k].y = a->ys[k];
	}
	{
		ExtrapolationResult pin_res = extrapolate_y_at_0(
			a->x, a->ys, a->nsamps,
			0.001,
			0.15);
		double y_pin_raw = pin_res.y_at_1;
		if (y_pin_raw < -1.1) y_pin_raw = -1.1;
		if (y_pin_raw >  1.1) y_pin_raw =  1.1;
		if (!a->s_y_pin_valid)
		{
			a->s_y_pin_ema   = y_pin_raw;
			a->s_y_pin_valid = 1;
			a->s_pin_cycle   = 1;
		} else
		{
			const int    PIN_WARMUP_CYCLES = 5;
			const double PIN_WARMUP_ALPHA  = 0.40;
			double eff_alpha;
			if (a->s_pin_cycle <= PIN_WARMUP_CYCLES)
				eff_alpha = PIN_WARMUP_ALPHA;
			else
				eff_alpha = (pin_res.confidence == EXTRAP_CONFIDENT)
					? a->pin_alpha : a->pin_alpha * 0.5;
			a->s_y_pin_ema = eff_alpha * y_pin_raw
				+ (1.0 - eff_alpha) * a->s_y_pin_ema;
			if (a->s_pin_cycle <= PIN_WARMUP_CYCLES) a->s_pin_cycle++;
		}
	}

	nf_default_config(a->s_config);
	a->s_config->ordering_mode       = NF_ORDER_BY_X;
	a->s_config->n_ctrl              = 20;
	a->s_config->adaptive_iters      = 0;
	a->s_config->outlier_sigma       = 2.5;
	a->s_config->local_outlier_iters = 0;
	a->s_config->y_min               = -1.05;
	a->s_config->y_max               = +1.05;
	a->s_config->fold_detect         = 1;
	a->s_config->pre_filter_x_min    = 0.02;
	a->s_config->pin_start           = 1;
	a->s_config->start_pt            = (NF_Point2){ 0.0, a->s_y_pin_ema };
	a->s_config->pin_end             = 0;
	a->s_ctrl_n = a->s_config->n_ctrl;

	if (eq_used) a->s_nurb = nf_fit(a->s_eqd, a->eq_n, a->s_config, a->s_nfres);
	else         a->s_nurb = nf_fit(a->s_data, a->nsamps, a->s_config, a->s_nfres);

	if (a->s_nurb == NULL)
	{
		a->binfo[3] |= 0b0001;
		goto cleanup;
	}
	if (a->s_nfres->quality & NF_FIT_BAD)
	{
		a->binfo[3] |= 0b0010;
		goto cleanup;
	}
	{
		const double alpha = 0.10;
		int nc = a->s_nurb->n_ctrl;
		if (!a->s_ctrl_ema_valid)
		{
			const double COLD_START_SIN_MAX = 1.1;
			double s_left_val = ns_eval_near_clamped(a->s_spline, 0.04,
			                        a->s_calavg.ys[0]);
			if (fabs(s_left_val) > COLD_START_SIN_MAX)
			{
				a->binfo[3] |= 0b00100000;
				goto cleanup;
			}
			for (int k = 0; k < nc; k++)
			{
				a->s_ctrl_ema_x[k] = a->s_nurb->ctrl_wx[k];
				a->s_ctrl_ema_y[k] = a->s_nurb->ctrl_wy[k];
			}
			a->s_ctrl_n         = nc;
			a->s_ctrl_ema_valid = 1;
		} else
		{
			for (int k = 0; k < nc; k++)
			{
				a->s_ctrl_ema_x[k] = alpha * a->s_nurb->ctrl_wx[k]
				                   + (1.0 - alpha) * a->s_ctrl_ema_x[k];
				a->s_ctrl_ema_y[k] = alpha * a->s_nurb->ctrl_wy[k]
				                   + (1.0 - alpha) * a->s_ctrl_ema_y[k];
			}
		}
		for (int k = 0; k < nc; k++)
		{
			a->s_nurb->ctrl_wx[k] = a->s_ctrl_ema_x[k];
			a->s_nurb->ctrl_wy[k] = a->s_ctrl_ema_y[k];
		}
	}

	a->s_spline = ns_build(a->s_nurb, a->s_nfres, 0);
	if (a->s_spline == NULL)
	{
		a->binfo[3] |= 0b0100;
		goto cleanup;
	}
	double s_max_err, s_rms_err;
	ns_accuracy_check(a->s_spline, a->s_nurb, 1000, &s_max_err, &s_rms_err);
	double s_noise_est = fmax(a->s_nfres->rms, a->s_nfres->cv_score);
	if (s_max_err > s_noise_est * 50.0)
	{
		a->binfo[3] |= 0b1000;

		ns_free(a->s_spline);
		a->s_spline = NULL;
		goto cleanup;
	}
	if (a->s_extend_left)
		ns_extend_left(a->s_spline, a->extend_x_target, a->s_anchor_ema,
		               a->extend_bound_frac, -1.1, 1.1);
	curve_ema_update(&a->s_calavg, a->s_spline);
	a->s_prev_y = a->s_calavg.ys[0];

	double prev_s = a->s_calavg.ys[0];
	double prev_c = a->c_calavg.ys[0];
	double max_identity_err = 0.0;
	for (int k = 0; k < 20; k++)
	{
		double x = 0.05 + 0.90 * k / 19.0;
		double s = ns_eval_near_clamped(a->s_spline, x, prev_s);
		double c = ns_eval_near_clamped(a->c_spline, x, prev_c);
		double err = fabs(s * s + c * c - 1.0);
		if (err > max_identity_err) max_identity_err = err;
		prev_s = s; prev_c = c;
	}
	if (max_identity_err > 0.05)
	{
		dprintf("Warning: sin²+cos² deviates by %.3f\n", max_identity_err);
		a->binfo[2] |= 0b00010000;
		a->binfo[3] |= 0b00010000;
		goto cleanup;
	}

	a->binfo[6] = 0b0000;
	{
		const double scheck_tol = 0.10;
		int scheck_fail = 0;
		double m_py = a->m_calavg.ys[0];
		double c_py = a->c_calavg.ys[0];
		double s_py = a->s_calavg.ys[0];
		for (int k = 0; k < SCHECK_PTS; k++)
		{
			double xk  = (double)k / (double)(SCHECK_PTS - 1);
			double m_y = ns_eval_near_clamped(a->m_spline, xk, m_py);
			double c_y = ns_eval_near_clamped(a->c_spline, xk, c_py);
			double s_y = ns_eval_near_clamped(a->s_spline, xk, s_py);
			m_py = m_y; c_py = c_y; s_py = s_y;
			if (a->scheck_valid)
			{
				if (fabs(m_y - a->m_prev_sol[k]) > scheck_tol) scheck_fail = 1;
				if (fabs(c_y - a->c_prev_sol[k]) > scheck_tol) scheck_fail = 1;
				if (fabs(s_y - a->s_prev_sol[k]) > scheck_tol) scheck_fail = 1;
			}
			a->m_prev_sol[k] = m_y;
			a->c_prev_sol[k] = c_y;
			a->s_prev_sol[k] = s_y;
		}
		a->scheck_valid = 1;
		if (scheck_fail)
		{
			a->binfo[6] |= 0b0001;
			goto cleanup;
		}
	}

	EnterCriticalSection (&a->disp.cs_disp);
	a->disp.nsamps = a->nsamps;
	memcpy(a->disp.x,  a->x,  a->nsamps * sizeof (double));
	memcpy(a->disp.ym, a->ym, a->nsamps * sizeof (double));
	memcpy(a->disp.yc, a->yc, a->nsamps * sizeof (double));
	memcpy(a->disp.ys, a->ys, a->nsamps * sizeof (double));

	a->disp.m_prev_y = a->m_calavg.ys[0];
	for (int k = 0; k < a->disp.cpts; k++)
	{
		double cx = (double)k / (double)(a->disp.cpts - 1);
		double y = get_mag_correction_ema(&a->m_calavg, cx, &a->disp.m_prev_y);
		a->disp.xm_cor[k] = cx;
		a->disp.ym_cor[k] = y;
	}
	a->disp.ym_cor[0] = a->disp.ym_cor[1];
	a->disp.c_prev_y = a->c_calavg.ys[0];
	for (int k = 0; k < a->disp.cpts; k++)
	{
		a->disp.xc_cor[k] = (double)k / (double)(a->disp.cpts - 1);
		a->disp.yc_cor[k] = get_phase_correction_ema(&a->c_calavg,
			a->disp.xc_cor[k], &a->disp.c_prev_y);
	}
	a->disp.s_prev_y = a->s_calavg.ys[0];
	for (int k = 0; k < a->disp.cpts; k++)
	{
		a->disp.xs_cor[k] = (double)k / (double)(a->disp.cpts - 1);
		a->disp.ys_cor[k] = get_phase_correction_ema(&a->s_calavg,
			a->disp.xs_cor[k], &a->disp.s_prev_y);
	}

	const double rad2deg = 180.0 / PI;
	a->disp.xa_cor[0] = 0.0;
	double sinval, cosval;
	sinval = a->disp.ys_cor[0];
	cosval = a->disp.yc_cor[0];
	a->disp.ya_cor[0] = rad2deg * atan2(sinval, cosval);
	for (int k = 1; k < a->disp.cpts; k++)
	{
		a->disp.xa_cor[k] = (double)k / (double)(a->disp.cpts - 1);
		sinval = a->disp.ys_cor[k];
		cosval = a->disp.yc_cor[k];
		double raw = rad2deg * atan2(sinval, cosval);
		double delta = raw - a->disp.ya_cor[k - 1];
		while (delta >  180.0) delta -= 360.0;
		while (delta < -180.0) delta += 360.0;
		a->disp.ya_cor[k] = a->disp.ya_cor[k - 1] + delta;
	}
	a->disp.phs_ref_deg = a->disp.ya_cor[a->disp.cpts - 1];
	double target_center_deg = 0.0;
	double angle_offset = target_center_deg - a->disp.ya_cor[a->disp.cpts - 1];
	for (int k = 0; k < a->disp.cpts; k++)
		a->disp.ya_cor[k] += angle_offset;
	LeaveCriticalSection (&a->disp.cs_disp);

cleanup:
	nf_curve_free(a->m_nurb);  a->m_nurb = NULL;
	nf_curve_free(a->c_nurb);  a->c_nurb = NULL;
	nf_curve_free(a->s_nurb);  a->s_nurb = NULL;

	a->scOK = ((a->binfo[0] == 0) && (a->binfo[1] == 0) && (a->binfo[2] == 0) &&
		(a->binfo[3] == 0) && (a->binfo[6] == 0));
	if (!a->scOK)
	{
		ns_free(a->m_spline); a->m_spline = NULL;
		ns_free(a->c_spline); a->c_spline = NULL;
		ns_free(a->s_spline); a->s_spline = NULL;
	}
	else
		a->binfo[5]++;
	return;
}

void __cdecl doPSCorrChange(void* arg)
{
	CALCC a = (CALCC)arg;
	uint32_t num_sems = 5;
	while (1)
	{
		uint32_t waitstat = WaitForMultipleObjects(num_sems, a->SemsPSCorr, FALSE, INFINITE);
		if ((waitstat >= WAIT_OBJECT_0) && (waitstat < (WAIT_OBJECT_0 + num_sems)))
		{
			uint32_t index = waitstat - WAIT_OBJECT_0;
			int error = 0;
			FILE* file = NULL;
			IQC b = NULL;
			switch (index)
			{
			case 0:
				SetTXAiqcEnd(a->channel);
				break;

			case 1:
				GetTXAiqcValues(a->channel, &a->util.m_spline_save, &a->util.m_calavg_save, &a->util.m_prev_y_save,
					&a->util.c_spline_save, &a->util.c_calavg_save, &a->util.c_prev_y_save,
					&a->util.s_spline_save, &a->util.s_calavg_save, &a->util.s_prev_y_save);
				error = WriteCorrectionFileV2(a->util.save_file,
					a->util.m_spline_save,
					&a->util.m_calavg_save,
					a->util.c_spline_save,
					&a->util.c_calavg_save,
					a->util.s_spline_save,
					&a->util.s_calavg_save);
				ns_free(a->util.m_spline_save); a->util.m_spline_save = NULL;
				ns_free(a->util.c_spline_save); a->util.c_spline_save = NULL;
				ns_free(a->util.s_spline_save); a->util.s_spline_save = NULL;
				if (error)
				{
					a->binfo[12] = 0b0001;
					EnterCriticalSection(&txa[a->channel].calcc.cs_update);
					a->info[12]  = 0b0001;
					LeaveCriticalSection(&txa[a->channel].calcc.cs_update);
				}
				break;

			case 2:
				error = ReadCorrectionFileV2(a->util.restore_file,
					&a->util.m_spline_restore,
					&a->util.m_calavg_restore,
					&a->util.c_spline_restore,
					&a->util.c_calavg_restore,
					&a->util.s_spline_restore,
					&a->util.s_calavg_restore);
				if (!error)
				{
					if (!InterlockedBitTestAndSet(&a->ctrl.running, 0))
					{
						SetTXAiqcStart(a->channel,
							a->util.m_spline_restore, &a->util.m_calavg_restore, a->util.m_prev_y_restore,
							a->util.c_spline_restore, &a->util.c_calavg_restore, a->util.c_prev_y_restore,
							a->util.s_spline_restore, &a->util.s_calavg_restore, a->util.s_prev_y_restore);
					}
					else
					{
						SetTXAiqcSwap(a->channel,
							a->util.m_spline_restore, &a->util.m_calavg_restore, a->util.m_prev_y_restore,
							a->util.c_spline_restore, &a->util.c_calavg_restore, a->util.c_prev_y_restore,
							a->util.s_spline_restore, &a->util.s_calavg_restore, a->util.s_prev_y_restore);
					}
					a->util.m_spline_restore = NULL;
					a->util.c_spline_restore = NULL;
					a->util.s_spline_restore = NULL;
				}
				else
				{
					a->binfo[12] = 0b0010;
					EnterCriticalSection(&txa[a->channel].calcc.cs_update);
					a->info[12]  = 0b0010;
					LeaveCriticalSection(&txa[a->channel].calcc.cs_update);
				}
				break;

			case 3:
				calc(a);
				if (a->scOK)
				{
					if (!InterlockedBitTestAndSet(&a->ctrl.running, 0))
					{
						SetTXAiqcStart(a->channel, a->m_spline, &a->m_calavg, a->m_prev_y,
							a->c_spline, &a->c_calavg, a->c_prev_y,
							a->s_spline, &a->s_calavg, a->s_prev_y);
					}
					else
					{
						SetTXAiqcSwap(a->channel, a->m_spline, &a->m_calavg, a->m_prev_y,
							a->c_spline, &a->c_calavg, a->c_prev_y,
							a->s_spline, &a->s_calavg, a->s_prev_y);
					}
					a->m_spline = NULL;
					a->c_spline = NULL;
					a->s_spline = NULL;
				}
				InterlockedBitTestAndSet(&a->ctrl.calcdone, 0);
				break;

			case 4:
				b = txa[a->channel].iqc.p;
				InterlockedBitTestAndReset(&b->busy, 0);
				SetEvent(a->hCorrChangeExited);
				return;

			default:

				break;
			}
		}
	}
}

enum _calcc_state
{
	LRESET,
	LWAIT,
	LMOXDELAY,
	LSETUP,
	LCOLLECT,
	MOXCHECK,
	LCALC,
	LDELAY,
	LSTAYON,
	LTURNON
};

#define TOP_BUCKET_PTS       100
#define DEADLOCK_MIN_FRAC    0.06

static double top_bucket_useful_frac(const CurveEMA* m, double bottom)
{
	double dummy = 0.0;
	int contiguous = 0;
	double frac_top_bucket = 0.0;
	for (int k = TOP_BUCKET_PTS - 1; k >= 0; k--)
	{
		double xk = bottom + (1.0 - bottom) * (double)k / (double)(TOP_BUCKET_PTS - 1);
		double zk = xk * get_mag_correction_ema(m, xk, &dummy);
		if (zk >= bottom) contiguous++;
		else break;
	}
	frac_top_bucket = (double)contiguous / (double)TOP_BUCKET_PTS;
	//dprintf("***** Top Bucket Available Fraction = %.4f\n", frac_top_bucket);
	return frac_top_bucket;
}

PORT
void pscc (int channel, int size, double* tx, double* rx)
{
	int i;
	CALCC a = txa[channel].calcc.p;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	if (a->runcal)
	{
		a->size = size;
		if (InterlockedAnd (&a->mox, 1) && (a->txdelay->tdelay != 0.0 || a->rxdelay->tdelay != 0.0))
		{
			SetDelayBuffs (a->rxdelay, a->size, rx, rx);
			xdelay (a->rxdelay);
			SetDelayBuffs (a->txdelay, a->size, tx, tx);
			xdelay (a->txdelay);
		}
		a->info[15] = a->ctrl.state;

		switch (a->ctrl.state)
		{
			case LRESET:
				InterlockedExchange (&a->ctrl.current_state, LRESET);
				if (!a->ctrl.calcinprogress)
				{
					ns_free(a->m_spline); a->m_spline = NULL;
					ns_free(a->c_spline); a->c_spline = NULL;
					ns_free(a->s_spline); a->s_spline = NULL;
					nf_curve_free(a->m_nurb); a->m_nurb = NULL;
					nf_curve_free(a->c_nurb); a->c_nurb = NULL;
					nf_curve_free(a->s_nurb); a->s_nurb = NULL;
				}
				a->m_prev_y = 1.0; a->c_prev_y = 1.0; a->s_prev_y = 0.0;
				curve_ema_init2(&a->m_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd, 0.1, 2.0);
				curve_ema_init2(&a->c_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd, -1.1, 1.1);
				curve_ema_init2(&a->s_calavg, a->ema_alpha, a->ema_alpha_lo, a->ema_x_bnd, -1.1, 1.1);
				a->m_fold_prev      = 0;
				a->m_ctrl_ema_valid = 0;
				a->c_ctrl_ema_valid = 0;
				a->s_ctrl_ema_valid = 0;
				a->c_y_pin_valid = 0; a->c_y_pin_ema = 1.0; a->c_pin_cycle = 0;
				a->s_y_pin_valid = 0; a->s_y_pin_ema = 0.0; a->s_pin_cycle = 0;
				a->scheck_valid = 0;
				a->ctrl.reset = 0;
				if (!a->ctrl.turnon)
					if (InterlockedBitTestAndReset(&a->ctrl.running, 0))
						ReleaseSemaphore(a->SemsPSCorr[0], 1, 0);
				a->info[14] = 0;
				a->ctrl.env_maxtx = 0.0;
				a->ctrl.bs_count = 0;
				if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (a->ctrl.automode || a->ctrl.mancal)
					a->ctrl.state = LWAIT;
				break;
			case LWAIT:
				InterlockedExchange (&a->ctrl.current_state, LWAIT);
				a->ctrl.mancal = 0;
				a->ctrl.moxcount = 0;
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (InterlockedAnd (&a->mox, 1))
					a->ctrl.state = LMOXDELAY;
				break;
			case LMOXDELAY:
				InterlockedExchange (&a->ctrl.current_state, LMOXDELAY);
				a->ctrl.moxcount += a->size;
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (!InterlockedAnd (&a->mox, 1))
					a->ctrl.state = LWAIT;
				else if ((a->ctrl.moxcount - a->size) >= a->ctrl.moxsamps)
					a->ctrl.state = LSETUP;
				break;
			case LSETUP:
				InterlockedExchange (&a->ctrl.current_state, LSETUP);
				a->ctrl.count = 0;
				a->ctrl.waitcount = 0;
				sampleCollectClear(&a->PS_Colct);
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (InterlockedAnd (&a->mox, 1))
				{
					a->ctrl.state = LCOLLECT;
				}
				else
					a->ctrl.state = LWAIT;
				break;
		    case LCOLLECT:
				InterlockedExchange (&a->ctrl.current_state, LCOLLECT);
				int full = 0;
				for (i = 0; i < a->size; i++)
				{
					putSample(&tx[2 * i], &rx[2 * i], a->hw_scale, &a->PS_Colct);
					full = sampleCheckAndUpdate(&a->PS_Colct);
					if (full) break;
				}
				a->ctrl.count += a->size;
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (!InterlockedAnd (&a->mox, 1))
					a->ctrl.state = LWAIT;
				else if (full)
					a->ctrl.state = MOXCHECK;
				else if (top_bucket_useful_frac(&a->m_calavg,
					a->PS_Colct.bbtm[NBUCKS - 1]) < DEADLOCK_MIN_FRAC)
				{
					a->ctrl.state = LRESET;
					a->info[6] |= 2;
				}
				else if (a->ctrl.count >= 5 * a->rate)
				{
					a->ctrl.count = 0;
					sampleCollectClear(&a->PS_Colct);
				}
				break;
			case MOXCHECK:
				InterlockedExchange (&a->ctrl.current_state, MOXCHECK);
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if (!InterlockedAnd (&a->mox, 1))
					a->ctrl.state = LWAIT;
				else
					a->ctrl.state = LCALC;
				break;
			case LCALC:
				InterlockedExchange (&a->ctrl.current_state, LCALC);
				if (!a->ctrl.calcinprogress)
				{
					a->ctrl.calcinprogress = 1;
					ReleaseSemaphore(a->SemsPSCorr[3], 1, 0);
				}
				if (InterlockedBitTestAndReset(&a->ctrl.calcdone, 0))
				{
					memcpy (a->info, a->binfo, 8 * sizeof (int));
					a->info[14] = _InterlockedAnd (&a->ctrl.running, 1);
					a->ctrl.calcinprogress = 0;
					if (a->ctrl.reset)
						a->ctrl.state = LRESET;
					else if (a->ctrl.turnon)
						a->ctrl.state = LTURNON;
					else if (a->scOK)
					{
						if (top_bucket_useful_frac(&a->m_calavg,
							a->PS_Colct.bbtm[NBUCKS - 1]) < DEADLOCK_MIN_FRAC)
						{
							a->ctrl.state = LRESET;
							a->info[6] |= 2;
						}
						else
						{
						a->ctrl.bs_count = 0;
						a->ctrl.state = LDELAY;
						}
					}
					else if (++(a->ctrl.bs_count) >= 3)
						a->ctrl.state = LRESET;
					else if (InterlockedAnd (&a->mox, 1))
						a->ctrl.state = LSETUP;
					else a->ctrl.state = LWAIT;
				}
				break;
			case LDELAY:
				InterlockedExchange (&a->ctrl.current_state, LDELAY);
				a->ctrl.waitcount += a->size;
				if (a->ctrl.reset)
					a->ctrl.state = LRESET;
				else if (a->ctrl.turnon)
					a->ctrl.state = LTURNON;
				else if ((a->ctrl.waitcount - a->size) >= a->ctrl.waitsamps)
				{
					if (a->ctrl.automode)
					{
						if (InterlockedAnd (&a->mox, 1))
							a->ctrl.state = LSETUP;
						else
							a->ctrl.state = LWAIT;
					}
					else
						a->ctrl.state = LSTAYON;
				}
				break;
			case LSTAYON:
				InterlockedExchange (&a->ctrl.current_state, LSTAYON);
				if (a->ctrl.reset || a->ctrl.automode || a->ctrl.mancal)
					a->ctrl.state = LRESET;
				break;
			case LTURNON:
				InterlockedExchange (&a->ctrl.current_state, LTURNON);
				a->ctrl.turnon = 0;
				a->ctrl.automode = 0;
				a->info[14] = 1;
				a->ctrl.state = LSTAYON;
				break;
		}
	}
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void PSSaveCorr (int channel, char* filename)
{
	CALCC a;
	int i = 0;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	while (a->util.save_file[i++] = *filename++);
	ReleaseSemaphore(a->SemsPSCorr[1], 1, 0);
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void PSRestoreCorr (int channel, char* filename)
{
	CALCC a;
	int i = 0;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	while (a->util.restore_file[i++] = *filename++);
	a->ctrl.turnon = 1;
	ReleaseSemaphore(a->SemsPSCorr[2], 1, 0);
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSRunCal (int channel, int run)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->runcal = run;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSMox (int channel, int mox)
{
	CALCC a = txa[channel].calcc.p;;
	if (mox)
		InterlockedBitTestAndSet (&a->mox, 0);
	else
		InterlockedBitTestAndReset (&a->mox, 0);
}

PORT
void GetPSInfo (int channel, int *info)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	memcpy (info, a->info, 16 * sizeof(int));
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSReset (int channel, int reset)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->ctrl.reset = reset;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSMancal (int channel, int mancal)
{
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	txa[channel].calcc.p->ctrl.mancal = mancal;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSAutomode (int channel, int automode)
{
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	txa[channel].calcc.p->ctrl.automode = automode;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSTurnon (int channel, int turnon)
{
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	txa[channel].calcc.p->ctrl.turnon = turnon;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSControl (int channel, int reset, int mancal, int automode, int turnon)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->ctrl.reset = reset;
	a->ctrl.mancal = mancal;
	a->ctrl.automode = automode;
	a->ctrl.turnon = turnon;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSLoopDelay (int channel, double delay)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->ctrl.loopdelay = delay;
	a->ctrl.waitsamps = (int)(a->rate * a->ctrl.loopdelay);
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void SetPSMoxDelay (int channel, double delay)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->ctrl.moxdelay = delay;
	a->ctrl.moxsamps = (int)(a->rate * a->ctrl.moxdelay);
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
double SetPSTXDelay (int channel, double delay)
{
	CALCC a;
	double adelay;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->txdel = delay;
	if (a->txdel >= 0.0)
	{
		adelay = SetDelayValue (a->txdelay, a->txdel);
		SetDelayValue (a->rxdelay, 0.0);
	}
	else
	{
		adelay = -SetDelayValue (a->rxdelay, -a->txdel);
		SetDelayValue (a->txdelay, 0.0);
	}
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
	return adelay;
}

PORT
void SetPSHWPeak (int channel, double peak)
{
	CALCC a;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a = txa[channel].calcc.p;
	a->hw_scale = 1.0 / peak;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void GetPSHWPeak (int channel, double* peak)
{
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	*peak = 1.0 / txa[channel].calcc.p->hw_scale;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void GetPSMaxTX (int channel, double* maxtx)
{
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	*maxtx = txa[channel].calcc.p->ctrl.env_maxtx;
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

PORT
void GetPSDisp (int channel, double* x, double* ym, double* yc, double* ys,
		double* xm_cor, double* ym_cor, double* xa_cor, double* ya_cor,
		int* nsamps_out, int* cpts_out, double* phs_ref_deg_out)
{
	CALCC a = txa[channel].calcc.p;
	EnterCriticalSection (&a->disp.cs_disp);
	int disp_nsamps = a->disp.nsamps;
	memcpy (x,  a->disp.x,  disp_nsamps * sizeof (double));
	memcpy (ym, a->disp.ym, disp_nsamps * sizeof (double));
	memcpy (yc, a->disp.yc, disp_nsamps * sizeof (double));
	memcpy (ys, a->disp.ys, disp_nsamps * sizeof (double));
	memcpy(xm_cor, a->disp.xm_cor, a->disp.cpts * sizeof(double));
	memcpy(ym_cor, a->disp.ym_cor, a->disp.cpts * sizeof(double));
	memcpy(xa_cor, a->disp.xa_cor, a->disp.cpts * sizeof(double));
	memcpy(ya_cor, a->disp.ya_cor, a->disp.cpts * sizeof(double));
	*nsamps_out      = disp_nsamps;
	*cpts_out        = a->disp.cpts;
	*phs_ref_deg_out = a->disp.phs_ref_deg;
	LeaveCriticalSection (&a->disp.cs_disp);
}

PORT
void SetPSFeedbackRate (int channel, int rate)
{
	CALCC a = txa[channel].calcc.p;
	EnterCriticalSection (&txa[channel].calcc.cs_update);
	a->rate = rate;
	a->ctrl.moxsamps = (int)(a->rate * a->ctrl.moxdelay);
	a->ctrl.waitsamps = (int)(a->rate * a->ctrl.loopdelay);
	destroy_delay (a->txdelay);
	destroy_delay (a->rxdelay);
	a->rxdelay = create_delay (
		1,
		0,
		0,
		0,
		a->rate,
		20.0e-09,
		0.0);
	a->txdelay = create_delay (
		1,
		0,
		0,
		0,
		a->rate,
		20.0e-09,
		a->txdel);
	LeaveCriticalSection (&txa[channel].calcc.cs_update);
}

void print_FitResult_and_Data(CALCC a, char* type, int printWhat)
{
	int rtype = 0;
	NF_FitResult* b = NULL;
	NS_Spline* c = NULL;
	NF_Curve* d = NULL;

	if (strcmp(type, "MAG") == 0)
	{
		b = a->m_nfres;
		c = a->m_spline;
		d = a->m_nurb;
		rtype = 1;
	}
	else if (strcmp(type, "COS") == 0)
	{
		b = a->c_nfres;
		c = a->c_spline;
		d = a->c_nurb;
		rtype = 2;
	}
	else if (strcmp(type, "SIN") == 0)
	{
		b = a->s_nfres;
		c = a->s_spline;
		d = a->s_nurb;
		rtype = 3;
	}
	else
	{
		dprintf("INVALID TYPE SPECIIED FOR 'print_FitResult_and_Data()' DIAGNOSTIC\n");
		return;
	}

	int print_DataPoints = printWhat & 0b0001;
	int print_FitResult = printWhat & 0b0010;
	int print_SplineDisp = printWhat & 0b0100;

	char* filename = NULL;

	if (print_DataPoints)
	{
		filename = NULL;
		if (rtype == 1) filename = "DataPoints-MAG.txt";
		if (rtype == 2) filename = "DataPoints-COS.txt";
		if (rtype == 3) filename = "DataPoints-SIN.txt";

		FILE* dfile = fopen(filename, "w");
		if (dfile)
		{
			for (int k = 0; k < a->nsamps; k++)
			{
				if (rtype == 1) fprintf(dfile, "%.6e     %.6e\n", a->x[k], a->ym[k]);
				if (rtype == 2) fprintf(dfile, "%.6e     %.6e\n", a->x[k], a->yc[k]);
				if (rtype == 3) fprintf(dfile, "%.6e     %.6e\n", a->x[k], a->ys[k]);
			}
			fflush(dfile);
			fclose(dfile);
		}
	}

	if (print_FitResult)
	{
		filename = NULL;
		if (rtype == 1) filename = "FitResult-MAG.txt";
		if (rtype == 2) filename = "FitResult-COS.txt";
		if (rtype == 3) filename = "FitResult-SIN.txt";
		FILE* file = fopen(filename, "w");
		if (file)
		{
			char bit_str1[64] = { 0 };
			char bit_str2[64] = { 0 };
			fprintf(file, "%s CURVE FAILURE\n\n", type);
			fprintf(file, "Data points:  %d\n", a->nsamps);
			fprintf(file, "binfo[%d] = %s\n", rtype, uint32_to_bitstr(a->binfo[rtype], bit_str1));

			double m_max_err, m_rms_err;
			ns_accuracy_check(c, d, 1000, &m_max_err, &m_rms_err);
			double m_noise_est = fmax(b->rms, b->cv_score);
			fprintf(file, "Spline max_err=%.2e is %.0fx noise (%.2e)\n\n",
				m_max_err, m_max_err / m_noise_est, m_noise_est);

			if (b)
			{
				fprintf(file, "NF_FitResult:\n");
				fprintf(file, "     quality          = %s\n", uint32_to_bitstr(b->quality, bit_str2));
				fprintf(file, "     rms              = %.4e\n", b->rms);
				fprintf(file, "     rms_outlier      = %.4e\n", b->rms_outlier);
				fprintf(file, "     n_outliers       = %d\n", b->n_outliers);
				fprintf(file, "     n_ctrl_final     = %d\n", b->n_ctrl_final);
				fprintf(file, "     n_ctrl_initial   = %d\n", b->n_ctrl_initial);
				fprintf(file, "     fold_detected    = %d\n", b->fold_detected);
				fprintf(file, "     fold_x_end       = %.4e\n", b->fold_x_end);
				fprintf(file, "     condition_number = %.4e\n", b->condition_number);
				fprintf(file, "     cv_score         = %.4e\n", b->cv_score);
				fprintf(file, "     ordering used    = %d\n", b->ordering_used);
				fprintf(file, "     spearman_rho     = %.4e\n", b->spearman_rho);
				if (b->quality & NF_FIT_BAD_OVERFIT)
					fprintf(file, "\nSEVERE OVERFIT: cv/rms = %.1f\n", b->cv_score / b->rms);
			}
			else
			{
				fprintf(file, "NF_FitResult is NULL\n");
			}
			if (c)
			{
				fprintf(file, "\nNS_Spline:\n");
				fprintf(file, "     n_branches   = %d\n", c->n_branches);
				fprintf(file, "     branch0_npts = %d\n", c->branches[0].n_pts);
				fprintf(file, "     xlo          = %.5f\n", c->branches[0].xs[0]);
				CurveEMA* ema     = (rtype==1) ? &a->m_calavg :
				                    (rtype==2) ? &a->c_calavg : &a->s_calavg;
				double pin_val    = (rtype==2) ? a->c_y_pin_ema : a->s_y_pin_ema;
				int    pin_valid  = (rtype==2) ? a->c_y_pin_valid : a->s_y_pin_valid;
				int    ctrl_valid = (rtype==1) ? a->m_ctrl_ema_valid :
				                    (rtype==2) ? a->c_ctrl_ema_valid : a->s_ctrl_ema_valid;
				fprintf(file, "\nCurveEMA (%s):\n", type);
				fprintf(file, "     count           = %d\n",   ema->count);
				fprintf(file, "     alpha           = %.3f\n", ema->alpha);
				fprintf(file, "     alpha_lo        = %.3f\n", ema->alpha_lo);
				fprintf(file, "     x_bnd           = %.3f\n", ema->x_alpha_boundary);
				fprintf(file, "\nControl-point EMA:\n");
				fprintf(file, "     ctrl_ema_valid  = %d\n",   ctrl_valid);
				fprintf(file, "\nLeft-end pin:\n");
				fprintf(file, "     pin_valid       = %d\n",   pin_valid);
				fprintf(file, "     pin_ema         = %.5f\n", pin_val);
				fprintf(file, "     scheck_valid    = %d\n", a->scheck_valid);
			}
			else
			{
				fprintf(file, "NS_Spline is NULL\n");
			}
			fflush(file);
			fclose(file);
		}
	}

	if (print_SplineDisp)
	{
		filename = NULL;
		if (rtype == 1) filename = "Spline-MAG.txt";
		if (rtype == 2) filename = "Spline-COS.txt";
		if (rtype == 3) filename = "Spline-SIN.txt";
		FILE* sfile = fopen(filename, "w");
		if (sfile)
		{
			for (int k = 0; k < a->disp.cpts; k++)
			{
				if (rtype == 1)
					fprintf(sfile, "%.5e     %.5e\n", a->disp.xm_cor[k], a->disp.ym_cor[k]);
				if (rtype == 2)
					fprintf(sfile, "%.5e     %.5e\n", a->disp.xc_cor[k], a->disp.yc_cor[k]);
				if (rtype == 3)
					fprintf(sfile, "%.5e     %.5e\n", a->disp.xs_cor[k], a->disp.ys_cor[k]);
			}
			fflush(sfile);
			fclose(sfile);
		}
	}
}

void print_OriginalAndFitSamples (CALCC a)
{
	FILE* f = NULL;
	char* filename = "OriginalSamples.txt";
	f = fopen(filename, "w");
	if (f)
	{
		fprintf(f, "hw_scale = %12.4e\n", a->hw_scale);
		fprintf(f, "rx_scale = %12.4e\n", a->rx_scale);
		fprintf(f, "\n buck      env_tx          env_rx                 ");
		fprintf(f, "x              ym             yc            ys\n");
		for (int i = 0, k = 0; i < NBUCKS; i++)
		{
			for (int j = 0; j < a->PS_Colct.tpb[i]; j++)
			{
				fprintf(f, "%5d    %12.4e   %12.4e        %12.4e   %12.4e   %12.4e   %12.4e\n", i,
					a->PS_Colct.smps[k].envTX, a->PS_Colct.smps[k].envRX,
					a->x[k], a->ym[k], a->yc[k], a->ys[k]);
				k++;
			}
		}
		fflush(f);
		fclose(f);
	}
}
