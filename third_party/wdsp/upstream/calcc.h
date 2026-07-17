/*  calcc.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2023, 2026 Warren Pratt, NR0V

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

#ifndef _calcc_h
#define _calcc_h
#include "delay.h"
#include "nurbs_spline.h"
#include "nurbs_fit.h"

#define NBUCKS				16
#define ACCEPT_OVERRANGE	1

typedef struct _cpt
{
	double I;
	double Q;
}cpt;

typedef struct _psSample
{
	cpt tx;
	cpt rx;
	double envTX;
	double envRX;
}psSample;

typedef struct _psCollection
{
	psSample* smps;
	int nsamps;
	int tpb[NBUCKS];
	double bbtm[NBUCKS+1];
	int bidx[NBUCKS];
	int cpb[NBUCKS];
	int nidx[NBUCKS];
	int bfull[NBUCKS];
	int nfull;
} psCollection;

typedef struct _calcc
{
	int channel;
	int runcal;
	int size;
	volatile long mox;
	int rate;
	int nsamps;
	volatile int scOK;
	double hw_scale;
	double rx_scale;

	psCollection PS_Colct;

	double* env_TX;
	double* env_RX;
	double* x;
	double* ym;
	double* yc;
	double* ys;

	int* info;
	int* binfo;
	double txdel;

	HANDLE SemsPSCorr[5];

	NF_Config    *m_config, *c_config, *s_config;
	NF_Point2    *m_data,   *c_data,   *s_data;
	NF_Curve     *m_nurb,   *c_nurb,   *s_nurb;
	NF_FitResult *m_nfres,  *c_nfres,  *s_nfres;
	NS_Spline    *m_spline, *c_spline, *s_spline;
	double        m_prev_y,  c_prev_y,  s_prev_y;
	CurveEMA      m_calavg,  c_calavg,  s_calavg;

	int     m_fold_prev;
	int     m_ctrl_n;
	double *m_ctrl_ema_x;
	double *m_ctrl_ema_y;
	int     m_ctrl_ema_valid;
	int     c_ctrl_n;
	double *c_ctrl_ema_x;
	double *c_ctrl_ema_y;
	int     c_ctrl_ema_valid;
	int     s_ctrl_n;
	double *s_ctrl_ema_x;
	double *s_ctrl_ema_y;
	int     s_ctrl_ema_valid;

	int     m_extend_left;
	int     c_extend_left;
	int     s_extend_left;

	double  extend_bound_frac;
	double  extend_x_target;

	int     dcb_enabled;
	double  dcb_thresh;
	double  dcb_cap;
	double  dcb_floor;
	int     dcb_nbins;
	int     dcb_confirm;
	int     dcb_min_per_bin;
	double  dcb_alpha;

	int        eq_enable;
	int        eq_nbins;
	int        eq_min_pts;
	int        eq_min_cnt;
	double     eq_robust_x;
	double     eq_rxmin;
	int        eq_n;
	NF_Point2* m_eqd;
	NF_Point2* c_eqd;
	NF_Point2* s_eqd;

	double  m_anchor_ema;
	double  c_anchor_ema;
	double  s_anchor_ema;
	int     m_anchor_valid;
	int     c_anchor_valid;
	int     s_anchor_valid;

	double  ema_alpha;
	double  ema_alpha_lo;
	double  ema_x_bnd;
	double  pin_alpha;

	double  c_y_pin_ema;
	int     c_y_pin_valid;
	int     c_pin_cycle;
	double  s_y_pin_ema;
	int     s_y_pin_valid;
	int     s_pin_cycle;

	double *m_prev_sol;
	double *c_prev_sol;
	double *s_prev_sol;
	int     scheck_valid;

	struct _ctrl
	{
		double moxdelay;
		double loopdelay;
		int state;
		int reset;
		int automode;
		int mancal;
		int turnon;
		int moxsamps;
		int moxcount;
		int count;
		int calcinprogress;
		volatile LONG calcdone;
		int waitsamps;
		int waitcount;
		double env_maxtx;
		volatile long running;
		int bs_count;
		volatile long current_state;
	} ctrl;
	struct _disp
	{
		int cpts;
		double* x;
		double* ym;
		double* yc;
		double* ys;
		double* xm_cor;
		double* ym_cor;
		double* xc_cor;
		double* yc_cor;
		double* xs_cor;
		double* ys_cor;
		double* xa_cor;
		double* ya_cor;
		int     nsamps;

		double  m_prev_y;
		double  c_prev_y;
		double  s_prev_y;

		double  phs_ref_deg;
		CRITICAL_SECTION cs_disp;
	} disp;
	DELAY rxdelay;
	DELAY txdelay;
	struct _util
	{
		char restore_file[256];
		NS_Spline*    m_spline_restore;
		NS_Spline*    c_spline_restore;
		NS_Spline*    s_spline_restore;
		double        m_prev_y_restore, c_prev_y_restore, s_prev_y_restore;
		CurveEMA      m_calavg_restore, c_calavg_restore, s_calavg_restore;
		char save_file[256];
		NS_Spline*    m_spline_save;
		NS_Spline*    c_spline_save;
		NS_Spline*    s_spline_save;
		double        m_prev_y_save, c_prev_y_save, s_prev_y_save;
		CurveEMA      m_calavg_save, c_calavg_save, s_calavg_save;
	} util;
	HANDLE hCorrChangeExited;
} calcc, *CALCC;

extern CALCC create_calcc(int channel, int runcal, int size, int rate, double hw_scale,
	double moxdelay, double loopdelay, int mox);

extern void destroy_calcc (CALCC a);

extern void flush_calcc (CALCC a);

extern __declspec(dllexport) void pscc (int channel, int size, double* tx, double* rx);

extern void __cdecl doPSCorrChange(void* arg);

extern void print_FitResult_and_Data(CALCC a, char* type, int printWhat);

extern void print_OriginalAndFitSamples(CALCC a);

#endif

// 'info' assignments:
//		 0 - builder for rx_scale
//				0b0001 = Extrapolate_Res.confidence  ('0' good; '1' reverted to linear)
//		 1 - builder for cm
//				0b0001 = nf_fit() Failed.		 [NURB Curve]
//				0b0010 = nf_fit() Quality bad.   [NURB Curve] 
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = count_extrema() Failed. 
//		 2 - builder for cc
//              0b0001 = nf_fit() Failed.		 [NURB Curve]
// 			    0b0010 = nf_fit() Quality bad.   [NURB Curve]
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = sin^2 + cos^2 Error.
//		 3 - builder for cs
//              0b0001 = nf_fit() Failed.		 [NURB Curve]
// 			    0b0010 = nf_fit() Quality bad.   [NURB Curve]
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = sin^2 + cos^2 Error.
//          0b00100000 = SIN Cold Start Error
//		 4 - feedback level
//		 5 - count of successful calibrations
//		 6 - scheck() - Final Solutions Check
//              0b0001 = New-Old Soln Compare Check
// 			    0b0010 = Stuck, Can't FIll Buckets, Probable Over-Drive
//		 7 - count of attempted calibrations
// 
//      12 - file write/read error
//		13 - 
//		14 - indicates iqc_Run = 1
//		15 - control state
//
//
//      ** => A non-zero value in binfo[0], binfo[1], binfo[2], binfo[3], or binfo[6]
//            sets a->scOK==0 and the solution is rejected.
