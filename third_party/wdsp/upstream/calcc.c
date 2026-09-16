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
#include "delay.h"

typedef struct _psCollection* PSCOLLECTION;
typedef struct _eqdensity* EQDENSITY;
typedef struct _extrema* EXTREMA;
typedef struct _dcby* DCBY;

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

	PSCOLLECTION ps_colct;

	double* env_TX;
	double* env_RX;
	double* norm_TX;
	double* x;
	double* ym;
	double* yc;
	double* ys;

	int* info;
	int* binfo;
	double txdel;

	HANDLE SemsPSCorr[5];

	NF_Config* m_config, * c_config, * s_config;
	NF_Point2* m_data, * c_data, * s_data;
	NF_Curve* m_nurb, * c_nurb, * s_nurb;
	NF_FitResult* m_nfres, * c_nfres, * s_nfres;
	NS_Spline* m_spline, * c_spline, * s_spline;
	double        m_prev_y, c_prev_y, s_prev_y;
	CurveEMA      m_calavg, c_calavg, s_calavg;

	EXTRAP1 m_extrap1;
	EXTRAP0 m_extrap0;
	EXTRAP0 c_extrap0;
	EXTRAP0 s_extrap0;

	NS_WS   m_ns_ws;
	NS_WS   c_ns_ws;
	NS_WS   s_ns_ws;

	NF_WS   m_nf_ws;
	NF_WS   c_nf_ws;
	NF_WS   s_nf_ws;

	int     m_fold_prev;
	int     m_ctrl_n;
	double* m_ctrl_ema_x;
	double* m_ctrl_ema_y;
	int     m_ctrl_ema_valid;
	int     c_ctrl_n;
	double* c_ctrl_ema_x;
	double* c_ctrl_ema_y;
	int     c_ctrl_ema_valid;
	int     s_ctrl_n;
	double* s_ctrl_ema_x;
	double* s_ctrl_ema_y;
	int     s_ctrl_ema_valid;

	DCBY m_dcb;
	DCBY c_dcb;
	DCBY s_dcb;
	double* dcb_phasor_mag;

	EQDENSITY  eq_density;
	int        eq_n;
	NF_Point2* m_eqd;
	NF_Point2* c_eqd;
	NF_Point2* s_eqd;

	EXTREMA m_extrema;

	double  m_anchor_ema;
	double  c_anchor_ema;
	double  s_anchor_ema;
	int     m_anchor_valid;
	int     c_anchor_valid;
	int     s_anchor_valid;

	double  m_y_pin_ema;
	double  m_y_pin_try;
	int     m_y_pin_valid;
	int     m_pin_cycle;
	double  c_y_pin_ema;
	double  c_y_pin_try;
	int     c_y_pin_valid;
	int     c_pin_cycle;
	double  s_y_pin_ema;
	double  s_y_pin_try;
	int     s_y_pin_valid;
	int     s_pin_cycle;

	double* m_prev_sol;
	double* c_prev_sol;
	double* s_prev_sol;
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
		NS_Spline* m_spline_restore;
		NS_Spline* c_spline_restore;
		NS_Spline* s_spline_restore;
		double        m_prev_y_restore, c_prev_y_restore, s_prev_y_restore;
		CurveEMA      m_calavg_restore, c_calavg_restore, s_calavg_restore;
		char save_file[256];
		NS_Spline* m_spline_save;
		NS_Spline* c_spline_save;
		NS_Spline* s_spline_save;
		double        m_prev_y_save, c_prev_y_save, s_prev_y_save;
		CurveEMA      m_calavg_save, c_calavg_save, s_calavg_save;
	} util;
	HANDLE hCorrChangeExited;
} calcc, * CALCC;

void __cdecl doPSCorrChange(void* arg);

void print_FitResult_and_Data(CALCC a, char* type, int printWhat);

void print_OriginalAndFitSamples(CALCC a);

void print_EQ_Samples(CALCC a);

#define SAMPLE_NBUCKS			  16           
#define SAMPLE_ACCEPT_OVERRANGE	  1             
#define SAMPLE_MIN_X              0.0050         
#define VAR_FORCED_SLOPE          0.001         

#define DCB_ENABLED				  0       
#define DCB_THRESH				  0.05
#define DCB_CAP					  0.25
#define DCB_FLOOR				  0.03          
#define DCB_NBINS				  24
#define DCB_CONFIRM				  2
#define DCB_MIN_PER_BIN			  20
#define DCB_ALPHA				  0.30

#define EQ_ENABLE                 1            
#define EQ_MIN_PTS		          60            
#define EQ_NBINS                  100            
#define EQ_MODE                   0		         
#define EQ_ROBUST_X               0.20	        
#define EQ_XMIN                   0.02	       
#define EQ_MIN_CNT	              3                
#define EXTP0_PIN_X_LO            0.01
#define EXTP0_PIN_X_HEAD_MAX      0.20
#define EXTP0_PIN_WARMUP_CYCLES   5
#define EXTP0_PIN_WARMUP_ALPHA    0.40
#define EXTP0_PIN_ALPHA           0.10

#define PS_NF_DEGREE              3                        
#define PS_NF_N_CTRL              20         
#define PS_NF_CTRL_MAX            200          
#define PS_NF_ORDERING_MODE       NF_ORDER_BY_X         
#define PS_NF_SPEARMAN_THRESH     0.85             
#define PS_NF_MAG_PREFILT_XMIN    0.04                    
#define PS_NF_MAG_PREFILT_YMAX    1.8                     
#define PS_NF_PHS_PREFILT_XMIN    0.02                  
#define PS_NF_PHS_PREFILT_YMAX    0.0                         

#define PS_NF_UNIFORM_KNOTS       1          
#define PS_NF_PIN_START           1                       
#define PS_NF_PIN_START_X         0.0          
#define PS_NF_MAG_PIN_END         1                            
#define PS_NF_MAG_END_X           1.0           
#define PS_NF_MAG_END_Y           1.0
#define PS_NF_PHS_PIN_END         0                       
#define PS_NF_XWEIGHT_X0          0.0             
#define PS_NF_XWEIGHT_MIN         0.04         

#define PS_NF_OUTLIER_ITERS       2                              
#define PS_NF_OUTLIER_SIGMA       2.5       
#define PS_NF_OUTLIER_MIN_FRAC    0.5           

#define PS_NF_CV_FRACTION         0.1                         
#define PS_NF_CV_OVERFIT_RATIO    1.5                 
#define PS_NF_CV_FATAL_RATIO      10.0                 

#define PS_NF_LOCAL_OUTLIER_ITERS 0                      
#define PS_NF_LOCAL_OUTLIER_SIGMA 4.0                      
#define PS_NF_LOCAL_OUTLIER_BANDS 20                               

#define PS_NF_FOLD_DETECT         1                   

#define PS_NF_ADAPTIVE_ITERS      0                     
#define PS_NF_ADAPTIVE_THRESH     2.0            
#define PS_NF_REPARAM_ITERS       2         
#define PS_NF_MIN_PTS_PER_CTRL    20         
#define PS_NF_IRLS_ITERS          2                          
#define PS_NF_IRLS_EPSILON        1e-6                     

#define PS_NF_MAG_YMIN            0.0                   
#define PS_NF_MAG_YMAX            0.0
#define PS_NF_PHS_YMIN           (-1.05)         
#define PS_NF_PHS_YMAX           (+1.05)

#define PS_NF_EMA_MAX_CTRL        32          

#define PS_NS_EXTEND_LEFT_MODE    0
#define PS_NS_EXTEND_BOUND_FRAC   0.05
#define PS_NS_EXTEND_X_TARGET     0.00
#define PS_NS_EMA_ALPHA           0.30
#define PS_NS_EMA_ALPHA_LO        0.30
#define PS_NS_EMA_X_BND           0.20

#define EXTREMA_CHECK             0.05		   
#define EXTREMA_STEPS             100		       
#define EXTREMA_X_LO              0.05		       
#define EXTREMA_X_HI              0.85		       

#define SCHECK_PTS                40             
#define SCHECK_TOL                0.10        
#define TOP_BUCKET_PTS            100          
#define DEADLOCK_MIN_FRAC         0.06           
#define SC_IDENTITY_PTS           20           
#define SC_IDENTITY_TOL		      0.05        

#define DISP_PTS                  512            


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
	double normTX;
}psSample;

typedef struct _psCollection
{
	psSample* smps;
	int nsamps;
	int tpb    [SAMPLE_NBUCKS];
	double bbtm[SAMPLE_NBUCKS + 1];
	int bidx   [SAMPLE_NBUCKS];
	int cpb    [SAMPLE_NBUCKS];
	int nidx   [SAMPLE_NBUCKS];
	int bfull  [SAMPLE_NBUCKS];
	int nfull;
} psCollection, *PSCOLLECTION;

static PSCOLLECTION build_collection()
{
	PSCOLLECTION collect = (PSCOLLECTION)malloc0(sizeof(psCollection));
	const double s_minx = SAMPLE_MIN_X;
	collect->bbtm[ 0] = s_minx; collect->tpb[ 0] = 256;
	collect->bbtm[ 1] = 0.0625; collect->tpb[ 1] = 256;
	collect->bbtm[ 2] = 0.1250; collect->tpb[ 2] = 256;
	collect->bbtm[ 3] = 0.1875; collect->tpb[ 3] = 256;
	collect->bbtm[ 4] = 0.2500; collect->tpb[ 4] = 256;
	collect->bbtm[ 5] = 0.3125; collect->tpb[ 5] = 256;
	collect->bbtm[ 6] = 0.3750; collect->tpb[ 6] = 256;
	collect->bbtm[ 7] = 0.4375; collect->tpb[ 7] = 256;
	collect->bbtm[ 8] = 0.5000; collect->tpb[ 8] = 256;
	collect->bbtm[ 9] = 0.5625; collect->tpb[ 9] = 256;
	collect->bbtm[10] = 0.6250; collect->tpb[10] = 256;
	collect->bbtm[11] = 0.6875; collect->tpb[11] = 256;
	collect->bbtm[12] = 0.7500; collect->tpb[12] = 256;
	collect->bbtm[13] = 0.8125; collect->tpb[13] = 256;
	collect->bbtm[14] = 0.8750; collect->tpb[14] = 256;
	collect->bbtm[15] = 0.9375; collect->tpb[15] = 256;
	collect->bbtm[SAMPLE_NBUCKS] = 1.0;
	int n = 0;
	for (int i = 0; i < SAMPLE_NBUCKS; i++)
	{
		collect->bidx[i] = n;
		collect->nidx[i] = 0;
		collect->cpb[i] = 0;
		collect->bfull[i] = 0;
		n += collect->tpb[i];
	}
	collect->nsamps = n;
	collect->smps = (psSample*)malloc0(n * sizeof(psSample));
	collect->nfull = 0;
	return collect;
}

static void teardown_collection(PSCOLLECTION collect)
{
	_aligned_free(collect->smps);
	_aligned_free(collect);
}

static int find_range_index(const double bounds[], int n, double val)
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

static void putSample(double* tx, double* rx, double hw_scale, PSCOLLECTION Collect)
{
	double env_tx = sqrt(tx[0] * tx[0] + tx[1] * tx[1]);
	double env_rx = sqrt(rx[0] * rx[0] + rx[1] * rx[1]);
	if (env_tx < 1.0e-30 || env_rx < 1.0e-30) return;
	double norm_tx = env_tx * hw_scale;
	int buck = find_range_index(Collect->bbtm, SAMPLE_NBUCKS + 1, norm_tx);
	if (buck < 0) return;
	if (buck >= SAMPLE_NBUCKS)
	{
		if (SAMPLE_ACCEPT_OVERRANGE)  buck = SAMPLE_NBUCKS - 1;
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
	Collect->smps[index_to_fill].normTX = norm_tx;
}

static int sampleCheckAndUpdate(PSCOLLECTION Collect)
{
	int rval = 0;
	if (Collect->nfull == SAMPLE_NBUCKS)
	{
		rval = 1;
		Collect->nfull = 0;
		for (int i = 0; i < SAMPLE_NBUCKS; i++)
		{
			Collect->nidx[i] = 0;
			Collect->cpb[i] = 0;
			Collect->bfull[i] = 0;
		}
	}
	return rval;
}

static void sampleCollectClear(PSCOLLECTION Collect)
{
	Collect->nfull = 0;
	for (int i = 0; i < SAMPLE_NBUCKS; i++)
	{
		Collect->nidx[i] = 0;
		Collect->cpb[i] = 0;
		Collect->bfull[i] = 0;
	}
}


typedef struct _eqdensity
{
	int* cnt;		         
	double* sx;		         
	double* sm;		         
	double* sc;		         
	double* ss;		         
	int nb_lo;		         
	int lo_max;	              
	int* offs;
	int* fill;
	double* lm;		       
	double* lc;
	double* ls;
	double* med_m;	       
	double* med_c;	       
	double* med_s;	       
} eqdensity, * EQDENSITY;

static EQDENSITY build_eqdensity(CALCC c)
{
	EQDENSITY a = (EQDENSITY)malloc0(sizeof(eqdensity));
	a->cnt = (int*)   malloc0(EQ_NBINS * sizeof(int));
	a->sx  = (double*)malloc0(EQ_NBINS * sizeof(double));
	a->sm  = (double*)malloc0(EQ_NBINS * sizeof(double));
	a->sc  = (double*)malloc0(EQ_NBINS * sizeof(double));
	a->ss  = (double*)malloc0(EQ_NBINS * sizeof(double));
	if (EQ_MODE == 1)	         
	{
		a->nb_lo = (int)(EQ_ROBUST_X * (double)EQ_NBINS + 0.5);
		a->lo_max = c->nsamps;        
	}
	else         
	{
		a->nb_lo = 0;
		a->lo_max = 0;
	}
	if (a->nb_lo > 0 && a->lo_max > 0)
	{
		a->offs  = (int*)   malloc0((EQ_NBINS + 1) * sizeof(int));
		a->fill  = (int*)   malloc0((EQ_NBINS)     * sizeof(int));
		a->lm    = (double*)malloc0(a->lo_max      * sizeof(double));
		a->lc    = (double*)malloc0(a->lo_max      * sizeof(double));
		a->ls    = (double*)malloc0(a->lo_max      * sizeof(double));
		a->med_m = (double*)malloc0(a->nb_lo       * sizeof(double));
		a->med_c = (double*)malloc0(a->nb_lo       * sizeof(double));
		a->med_s = (double*)malloc0(a->nb_lo       * sizeof(double));
	}
	else
	{
		a->offs = NULL; a->fill = NULL;
		a->lm = NULL; a->lc = NULL; a->ls = NULL;
		a->med_m = NULL; a->med_c = NULL; a->med_s = NULL;
	}
	return a;
}

static void teardown_eqdensity(EQDENSITY a)
{
	if (a->med_s) _aligned_free(a->med_s);
	if (a->med_c) _aligned_free(a->med_c);
	if (a->med_m) _aligned_free(a->med_m);
	if (a->ls)    _aligned_free(a->ls);
	if (a->lc)    _aligned_free(a->lc);
	if (a->lm)    _aligned_free(a->lm);
	if (a->fill)  _aligned_free(a->fill);
	if (a->offs)  _aligned_free(a->offs);
	_aligned_free(a->ss);
	_aligned_free(a->sc);
	_aligned_free(a->sm);
	_aligned_free(a->sx);
	_aligned_free(a->cnt);
	_aligned_free(a);
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
	EQDENSITY e = a->eq_density;
	const int nb = EQ_NBINS;
	memset(e->cnt, 0, nb * sizeof(int));
	memset(e->sx,  0, nb * sizeof(double));
	memset(e->sm,  0, nb * sizeof(double));
	memset(e->sc,  0, nb * sizeof(double));
	memset(e->ss,  0, nb * sizeof(double));
	for (int i = 0; i < a->nsamps; i++)		    
	{
		if (a->x[i] < EQ_XMIN) continue;            
		int b = (int)(a->x[i] * nb);		    							
		if (b < 0)      b = 0;				     
		if (b > nb - 1) b = nb - 1;
		e->cnt[b]++;						       
		e->sx[b] += a->x[i];				         
		e->sm[b] += a->ym[i];				         
		e->sc[b] += a->yc[i];				         
		e->ss[b] += a->ys[i];				         
	}
	if (e->nb_lo > 0)	     
	{
		e->offs[0] = 0;
		for (int b = 0; b < e->nb_lo; b++) e->offs[b + 1] = e->offs[b] + e->cnt[b];
		memset(e->fill, 0, EQ_NBINS * sizeof(int));
		for (int i = 0; i < a->nsamps; i++)		    
		{
			if (a->x[i] < EQ_XMIN) continue;            
			int b = (int)(a->x[i] * nb);			    	
			if (b < 0)      b = 0;					     
			if (b > nb - 1) b = nb - 1;
			if (b < e->nb_lo)						         
			{
				int idx = e->offs[b] + e->fill[b]++;          
				e->lm[idx] = a->ym[i];				          
				e->lc[idx] = a->yc[i];
				e->ls[idx] = a->ys[i];
			}
		}
		for (int b = 0; b < e->nb_lo; b++)	        
		{
			if (e->cnt[b] >= 3)				        
			{
				e->med_m[b] = eq_median(e->lm + e->offs[b], e->cnt[b]);	      
				e->med_c[b] = eq_median(e->lc + e->offs[b], e->cnt[b]);	      
				e->med_s[b] = eq_median(e->ls + e->offs[b], e->cnt[b]);	      
			}
		}
	}
	int n = 0;						     
	for (int b = 0; b < nb; b++)	      
	{
		if (e->cnt[b] == 0) continue;			     
		if (e->cnt[b] < EQ_MIN_CNT) continue;	       
		double inv = 1.0 / (double)e->cnt[b];	        
		a->m_eqd[n].x = a->c_eqd[n].x = a->s_eqd[n].x = e->sx[b] * inv;
		a->m_eqd[n].y = e->sm[b] * inv;
		a->c_eqd[n].y = e->sc[b] * inv;
		a->s_eqd[n].y = e->ss[b] * inv;
		if (b < e->nb_lo && e->cnt[b] >= 3 && e->med_m != NULL)
		{
			a->m_eqd[n].y = e->med_m[b];
			a->c_eqd[n].y = e->med_c[b];
			a->s_eqd[n].y = e->med_s[b];
		}
		n++;	       
	}
	return n;
}

typedef struct _extrema
{
	double* ys;
} extrema, * EXTREMA;

static EXTREMA build_extrema()
{
	EXTREMA a = (EXTREMA)malloc0(sizeof(extrema));
	a->ys = (double*)malloc0(EXTREMA_STEPS * sizeof(double));
	return a;
}

static void teardown_extrema(EXTREMA a)
{
	_aligned_free(a->ys);
	_aligned_free(a);
}

static int count_extrema(EXTREMA e, const NS_Spline* s)
{
	const double x_lo = EXTREMA_X_LO;		       
	const double x_hi = EXTREMA_X_HI;		       
	const int n_steps = EXTREMA_STEPS;		       
	const double min_prom = EXTREMA_CHECK;	   
	if (!s || n_steps < 3) return 0;
	double prev_y = ns_eval_near_clamped(s, x_lo, 1.0);
	for (int i = 0; i < n_steps; i++)
	{
		double x = x_lo + (x_hi - x_lo) * i / (n_steps - 1);
		e->ys[i] = ns_eval_near_clamped(s, x, prev_y);
		prev_y = e->ys[i];
	}

	int    extrema = 0;									   
	int    dir = 0;										    
	double ext = e->ys[0];								   

	for (int i = 1; i < n_steps; i++)
	{
		double y = e->ys[i];
		if (dir == 0)									    
		{
			if (y > e->ys[0] + min_prom) dir = 1;		  
			else if (y < e->ys[0] - min_prom) dir = -1;	  
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
	return extrema;
}

typedef struct _dcby
{
	double* tmp;
	double* tmpd;
}dcby, *DCBY;

static DCBY build_dcb(int n)
{
	DCBY a   = (DCBY)malloc0(sizeof(dcby));
	a->tmp  = (double*)malloc0(n * sizeof(double));
	a->tmpd = (double*)malloc0(n * sizeof(double));
	return a;
}

static void teardown_dcb(DCBY a)
{
	_aligned_free(a->tmpd);
	_aligned_free(a->tmp);
	_aligned_free(a);
}

static int dcb_cmp_double(const void* pa, const void* pb)
{
	double a = *(const double*)pa, b = *(const double*)pb;
	return (a < b) ? -1 : (a > b) ? 1 : 0;
}

static double detect_clean_boundary(DCBY a, const double* x, const double* y,
	const double* denom, int n,
	double x_lo, double x_hi, int nbins,
	double thresh, int confirm,
	double floor_x, double cap_x,
	int min_per_bin)
{
	if (n < min_per_bin) return cap_x;

	double  bw = (x_hi - x_lo) / (double)nbins;
	
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
				a->tmp[m] = y[i];
				a->tmpd[m] = denom ? denom[i] : y[i];
				m++;
			}
		}
		if (m < min_per_bin) { run = 0; continue; }

		qsort(a->tmp, m, sizeof(double), dcb_cmp_double);
		double q1 = a->tmp[(int)(0.25 * (m - 1))];
		double q3 = a->tmp[(int)(0.75 * (m - 1))];
		double iqr = q3 - q1;

		qsort(a->tmpd, m, sizeof(double), dcb_cmp_double);
		double dmed = a->tmpd[m / 2];
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
	if (result < floor_x) result = floor_x;
	if (result > cap_x)   result = cap_x;
	return result;
}

static int scheck(CALCC a)
{
	int scheck_fail = 0;
	double m_py = a->m_calavg.ys[0];
	double c_py = a->c_calavg.ys[0];
	double s_py = a->s_calavg.ys[0];
	for (int k = 0; k < SCHECK_PTS; k++)
	{
		double xk = (double)k / (double)(SCHECK_PTS - 1);
		double m_y = ns_eval_near_clamped(a->m_spline, xk, m_py);
		double c_y = ns_eval_near_clamped(a->c_spline, xk, c_py);
		double s_y = ns_eval_near_clamped(a->s_spline, xk, s_py);
		m_py = m_y; c_py = c_y; s_py = s_y;
		if (a->scheck_valid)
		{
			if (fabs(m_y - a->m_prev_sol[k]) > SCHECK_TOL) scheck_fail = 1;
			if (fabs(c_y - a->c_prev_sol[k]) > SCHECK_TOL) scheck_fail = 1;
			if (fabs(s_y - a->s_prev_sol[k]) > SCHECK_TOL) scheck_fail = 1;
		}
		a->m_prev_sol[k] = m_y;
		a->c_prev_sol[k] = c_y;
		a->s_prev_sol[k] = s_y;
	}
	a->scheck_valid = 1;
	if (scheck_fail)
		dprintf("***** PS: Solution Deviation\n");
	return scheck_fail;
}

static int sin_cos_identity_check(CALCC a)
{
	int error = 0;
	double prev_s = a->s_calavg.ys[0];
	double prev_c = a->c_calavg.ys[0];
	double max_identity_err = 0.0;
	for (int k = 0; k < SC_IDENTITY_PTS; k++)
	{
		double x = 0.05 + 0.90 * k / (double)(SC_IDENTITY_PTS - 1);
		double s = ns_eval_near_clamped(a->s_spline, x, prev_s);
		double c = ns_eval_near_clamped(a->c_spline, x, prev_c);
		double err = fabs(s * s + c * c - 1.0);
		if (err > max_identity_err) max_identity_err = err;
		prev_s = s; prev_c = c;
	}
	if (max_identity_err > SC_IDENTITY_TOL) error = 1;
	if (error) 
		dprintf("***** PS: sin²+cos² deviates by %.3f\n", max_identity_err);
	return error;
}

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
	if(frac_top_bucket < DEADLOCK_MIN_FRAC)
		dprintf("***** PS: Top Bucket Available Fraction = %.4f\n", frac_top_bucket);
	return frac_top_bucket;
}

static void size_calcc (CALCC a)
{
	a->nsamps = a->ps_colct->nsamps;

	a->env_TX  = (double*)malloc0(a->nsamps * sizeof(double));
	a->env_RX  = (double*)malloc0(a->nsamps * sizeof(double));
	a->norm_TX = (double*)malloc0(a->nsamps * sizeof(double));
	a->x       = (double*)malloc0(a->nsamps * sizeof(double));
	a->ym      = (double*)malloc0(a->nsamps * sizeof(double));
	a->yc      = (double*)malloc0(a->nsamps * sizeof(double));
	a->ys      = (double*)malloc0(a->nsamps * sizeof(double));

	a->m_config = (NF_Config*)malloc0(sizeof(NF_Config));
	a->c_config = (NF_Config*)malloc0(sizeof(NF_Config));
	a->s_config = (NF_Config*)malloc0(sizeof(NF_Config));

	a->m_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));
	a->c_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));
	a->s_data   = (NF_Point2*)malloc0(a->nsamps * sizeof(NF_Point2));

	a->m_extrap1 = build_extrap1(a->nsamps);
	a->m_extrap0 = build_extrap0(a->nsamps);
	a->c_extrap0 = build_extrap0(a->nsamps);
	a->s_extrap0 = build_extrap0(a->nsamps);

	a->m_ns_ws = build_ns_ws(PS_NF_CTRL_MAX);
	a->c_ns_ws = build_ns_ws(PS_NF_CTRL_MAX);
	a->s_ns_ws = build_ns_ws(PS_NF_CTRL_MAX);

	a->m_nf_ws = build_nf_ws(a->nsamps, PS_NF_CTRL_MAX);
	a->c_nf_ws = build_nf_ws(a->nsamps, PS_NF_CTRL_MAX);
	a->s_nf_ws = build_nf_ws(a->nsamps, PS_NF_CTRL_MAX);

	a->eq_density = build_eqdensity(a);
	a->eq_n = 0;
	a->m_eqd = (NF_Point2*)malloc0(EQ_NBINS * sizeof(NF_Point2));
	a->c_eqd = (NF_Point2*)malloc0(EQ_NBINS * sizeof(NF_Point2));
	a->s_eqd = (NF_Point2*)malloc0(EQ_NBINS * sizeof(NF_Point2));

	a->m_extrema = build_extrema();

	a->m_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));
	a->c_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));
	a->s_nfres  = (NF_FitResult*)malloc0(sizeof(NF_FitResult));

	a->disp.nsamps = 0;
	a->disp.x  = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.ym = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.yc = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.ys = (double *) malloc0 (a->nsamps * sizeof (double));
	a->disp.xm_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.ym_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.xc_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.yc_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.xs_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.ys_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.xa_cor = (double *) malloc0 (DISP_PTS * sizeof (double));
	a->disp.ya_cor = (double *) malloc0 (DISP_PTS * sizeof (double));

	a->m_dcb = build_dcb(a->nsamps);
	a->c_dcb = build_dcb(a->nsamps);
	a->s_dcb = build_dcb(a->nsamps);
	a->dcb_phasor_mag  = (double*)malloc0(a->nsamps * sizeof(double));

	a->m_anchor_ema   = 0.04;        
	a->c_anchor_ema   = 0.04;        
	a->s_anchor_ema   = 0.04;        
	a->m_anchor_valid = 0;
	a->c_anchor_valid = 0;
	a->s_anchor_valid = 0;

	curve_ema_init2(&a->m_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND,  0.1, 2.0);
	curve_ema_init2(&a->c_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND, -1.1, 1.1);
	curve_ema_init2(&a->s_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND, -1.1, 1.1);

	a->m_fold_prev      = 0;
	a->m_ctrl_n         = 0;
	a->m_ctrl_ema_x     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->m_ctrl_ema_y     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->m_ctrl_ema_valid = 0;
	a->c_ctrl_n         = 0;
	a->c_ctrl_ema_x     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->c_ctrl_ema_y     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->c_ctrl_ema_valid = 0;
	a->s_ctrl_n         = 0;
	a->s_ctrl_ema_x     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->s_ctrl_ema_y     = (double*)malloc0(PS_NF_EMA_MAX_CTRL * sizeof(double));
	a->s_ctrl_ema_valid = 0;

	a->m_y_pin_ema   = 1.0;
	a->m_y_pin_try   = 1.0;
	a->m_y_pin_valid = 0;
	a->m_pin_cycle   = 0;
	a->c_y_pin_ema   = 1.0;
	a->c_y_pin_try   = 1.0;
	a->c_y_pin_valid = 0;
	a->c_pin_cycle   = 0;
	a->s_y_pin_ema   = 0.0;
	a->s_y_pin_try   = 0.0;
	a->s_y_pin_valid = 0;
	a->s_pin_cycle   = 0;

	a->m_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->c_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->s_prev_sol  = (double*)malloc0(SCHECK_PTS * sizeof(double));
	a->scheck_valid = 0;
}

static void desize_calcc (CALCC a)
{
	_aligned_free(a->dcb_phasor_mag);
	teardown_dcb(a->s_dcb);
	teardown_dcb(a->c_dcb);
	teardown_dcb(a->m_dcb);

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

	teardown_extrema(a->m_extrema);

	_aligned_free(a->s_eqd);
	_aligned_free(a->c_eqd);
	_aligned_free(a->m_eqd);
	teardown_eqdensity(a->eq_density);

	teardown_extrap1(a->m_extrap1);
	teardown_extrap0(a->m_extrap0);
	teardown_extrap0(a->c_extrap0);
	teardown_extrap0(a->s_extrap0);

	teardown_ns_ws(a->m_ns_ws);
	teardown_ns_ws(a->c_ns_ws);
	teardown_ns_ws(a->s_ns_ws);

	teardown_nf_ws(a->m_nf_ws);
	teardown_nf_ws(a->c_nf_ws);
	teardown_nf_ws(a->s_nf_ws);

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
	_aligned_free(a->norm_TX);
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

	a->ps_colct = build_collection();

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

	teardown_collection(a->ps_colct);

	_aligned_free (a);
}

void flush_calcc (CALCC a)
{
	flush_delay (a->rxdelay);
	flush_delay (a->txdelay);
}

static void calc (CALCC a)
{
	PSCOLLECTION b = a->ps_colct;

	a->binfo[0] = 0b0000;
	a->binfo[1] = 0x0000;
	a->binfo[2] = 0x0000;
	a->binfo[3] = 0x0000;
	a->binfo[6] = 0b0000;
	a->binfo[7]++;

	a->m_nurb = NULL;
	a->c_nurb = NULL;
	a->s_nurb = NULL;
	
	a->ctrl.env_maxtx = 0.0;
	for (int i = 0; i < a->nsamps; i++)
	{
		a->env_TX[i]  = b->smps[i].envTX;
		a->env_RX[i]  = b->smps[i].envRX;
		a->norm_TX[i] = b->smps[i].normTX;
		if (a->env_TX[i] > a->ctrl.env_maxtx) a->ctrl.env_maxtx = a->env_TX[i];
	}

	ExtrapolationResult Extrapolate_Res = extrapolate_y_at_1(a->m_extrap1, a->norm_TX, a->env_RX, a->nsamps);
	a->rx_scale = 1.0 / Extrapolate_Res.y_at_1;
	if (Extrapolate_Res.confidence)
	{
		a->binfo[0] |= 0b0001;
		goto cleanup;
	}

	a->binfo[4] = (int)(256.0 * (a->hw_scale / a->rx_scale));

	for (int i = 0; i < a->nsamps; i++)
	{
		const double slope = VAR_FORCED_SLOPE;
		double rx_c = a->env_RX[i];
		double max_rx = (1.0 - slope + slope * a->hw_scale * a->env_TX[i]) / a->rx_scale;
		if (rx_c > max_rx) rx_c = max_rx;
		a->x[i]  = a->rx_scale * rx_c;
		a->ym[i] = (a->hw_scale * a->env_TX[i]) / (a->rx_scale * rx_c);
		double norm = a->env_TX[i] * a->env_RX[i];
		a->yc[i] = (+ b->smps[i].tx.I * b->smps[i].rx.I
			        + b->smps[i].tx.Q * b->smps[i].rx.Q) / norm;
		a->ys[i] = (- b->smps[i].tx.I * b->smps[i].rx.Q
			        + b->smps[i].tx.Q * b->smps[i].rx.I) / norm;
	}

	if (DCB_ENABLED)
	{
		for (int i = 0; i < a->nsamps; i++)
			a->dcb_phasor_mag[i] = sqrt(a->yc[i]*a->yc[i] + a->ys[i]*a->ys[i]);

		double m_anc = detect_clean_boundary(a->m_dcb, a->x, a->ym, NULL, a->nsamps,
		                   0.0, DCB_CAP + 0.05, DCB_NBINS,
		                   DCB_THRESH, DCB_CONFIRM,
		                   DCB_FLOOR, DCB_CAP, DCB_MIN_PER_BIN);
		double c_anc = detect_clean_boundary(a->c_dcb, a->x, a->yc, a->dcb_phasor_mag, a->nsamps,
		                   0.0, DCB_CAP + 0.05, DCB_NBINS,
						   DCB_THRESH, DCB_CONFIRM,
						   DCB_FLOOR, DCB_CAP, DCB_MIN_PER_BIN);
		double s_anc = detect_clean_boundary(a->s_dcb, a->x, a->ys, a->dcb_phasor_mag, a->nsamps,
		                   0.0, DCB_CAP + 0.05, DCB_NBINS,
						   DCB_THRESH, DCB_CONFIRM,
						   DCB_FLOOR, DCB_CAP, DCB_MIN_PER_BIN);

		if (!a->m_anchor_valid) { a->m_anchor_ema = m_anc; a->m_anchor_valid = 1; }
		else a->m_anchor_ema = DCB_ALPHA * m_anc + (1.0 - DCB_ALPHA) * a->m_anchor_ema;
		if (!a->c_anchor_valid) { a->c_anchor_ema = c_anc; a->c_anchor_valid = 1; }
		else a->c_anchor_ema = DCB_ALPHA * c_anc + (1.0 - DCB_ALPHA) * a->c_anchor_ema;
		if (!a->s_anchor_valid) { a->s_anchor_ema = s_anc; a->s_anchor_valid = 1; }
		else a->s_anchor_ema = DCB_ALPHA * s_anc + (1.0 - DCB_ALPHA) * a->s_anchor_ema;
	}
	else
	{
		a->m_anchor_ema = DCB_FLOOR; a->m_anchor_valid = 0;
		a->c_anchor_ema = DCB_FLOOR; a->c_anchor_valid = 0;
		a->s_anchor_ema = DCB_FLOOR; a->s_anchor_valid = 0;
	}

	int eq_used = 0;
	if (EQ_ENABLE)
	{
		a->eq_n = equalize_density(a);
		if (a->eq_n >= EQ_MIN_PTS) eq_used = 1;
	}

	for (int k = 0; k < a->nsamps; k++)
	{
		a->m_data[k].x = a->x[k];
		a->m_data[k].y = a->ym[k];
	}
	{
		ExtrapolationResult pin_res = extrapolate_y_at_0(a->m_extrap0,
			a->x, a->ym, a->nsamps, EXTP0_PIN_X_LO, EXTP0_PIN_X_HEAD_MAX);
		double y_pin_raw = pin_res.y_at_1;
		if (y_pin_raw < 0.1) y_pin_raw = 0.1;
		if (y_pin_raw > 2.0) y_pin_raw = 2.0;
		if (!a->m_y_pin_valid)
		{
			a->m_y_pin_try   = y_pin_raw;
			a->m_pin_cycle   = 1;
		} else
		{
			double eff_alpha;
			if (a->m_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES)
				eff_alpha = EXTP0_PIN_WARMUP_ALPHA;
			else
				eff_alpha = (pin_res.confidence == EXTRAP_CONFIDENT)
					? EXTP0_PIN_ALPHA : EXTP0_PIN_ALPHA * 0.5;
			a->m_y_pin_try = eff_alpha * y_pin_raw
				+ (1.0 - eff_alpha) * a->m_y_pin_ema;
			if (a->m_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES) a->m_pin_cycle++;
		}
	}

	nf_default_config(a->m_config);
	a->m_config->degree              = PS_NF_DEGREE;
	a->m_config->n_ctrl              = PS_NF_N_CTRL;
	a->m_config->n_ctrl_max          = PS_NF_CTRL_MAX;
	a->m_config->ordering_mode       = PS_NF_ORDERING_MODE;
	a->m_config->spearman_threshold  = PS_NF_SPEARMAN_THRESH;
	a->m_config->pre_filter_x_min    = PS_NF_MAG_PREFILT_XMIN;
	a->m_config->pre_filter_y_max    = PS_NF_MAG_PREFILT_YMAX;
	a->m_config->uniform_knots       = PS_NF_UNIFORM_KNOTS;
	a->m_config->pin_start           = PS_NF_PIN_START;
	a->m_config->start_pt            = (NF_Point2){ PS_NF_PIN_START_X, a->m_y_pin_try };
	a->m_config->pin_end             = PS_NF_MAG_PIN_END;
	a->m_config->end_pt              = (NF_Point2){ PS_NF_MAG_END_X, PS_NF_MAG_END_Y };
	a->m_config->pin_end_horiz       = a->m_fold_prev;        
	a->m_config->pin_end_flat        = a->m_fold_prev;        
	a->m_config->pin_end_flat2       = a->m_fold_prev;        
	a->m_config->x_weight_x0         = PS_NF_XWEIGHT_X0;
	a->m_config->x_weight_min        = PS_NF_XWEIGHT_MIN;
	a->m_config->outlier_iters       = PS_NF_OUTLIER_ITERS;
	a->m_config->outlier_sigma       = PS_NF_OUTLIER_SIGMA;
	a->m_config->outlier_min_fraction= PS_NF_OUTLIER_MIN_FRAC;
	a->m_config->cv_fraction         = PS_NF_CV_FRACTION;
	a->m_config->cv_overfit_ratio    = PS_NF_CV_OVERFIT_RATIO;
	a->m_config->cv_fatal_ratio      = PS_NF_CV_FATAL_RATIO;
	a->m_config->local_outlier_iters = PS_NF_LOCAL_OUTLIER_ITERS;
	a->m_config->local_outlier_sigma = PS_NF_LOCAL_OUTLIER_SIGMA;
	a->m_config->local_outlier_bands = PS_NF_LOCAL_OUTLIER_BANDS;
	a->m_config->fold_detect         = PS_NF_FOLD_DETECT;
	a->m_config->adaptive_iters      = PS_NF_ADAPTIVE_ITERS;
	a->m_config->adaptive_threshold  = PS_NF_ADAPTIVE_THRESH;
	a->m_config->reparam_iters       = PS_NF_REPARAM_ITERS;
	a->m_config->min_pts_per_ctrl    = PS_NF_MIN_PTS_PER_CTRL;
	a->m_config->irls_iters          = PS_NF_IRLS_ITERS;
	a->m_config->irls_epsilon        = PS_NF_IRLS_EPSILON;
	a->m_config->y_min               = PS_NF_MAG_YMIN;
	a->m_config->y_max               = PS_NF_MAG_YMAX;
	a->m_ctrl_n = a->m_config->n_ctrl;

	if (eq_used) a->m_nurb = nf_fit(a->m_nf_ws, a->m_eqd,  a->eq_n,   a->m_config, a->m_nfres);
	else         a->m_nurb = nf_fit(a->m_nf_ws, a->m_data, a->nsamps, a->m_config, a->m_nfres);

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
	
	for (int k = 0; k < a->nsamps; k++)
	{
		a->c_data[k].x = a->x[k];
		a->c_data[k].y = a->yc[k];
	}
	{
		ExtrapolationResult pin_res = extrapolate_y_at_0( a->c_extrap0,
			a->x, a->yc, a->nsamps, EXTP0_PIN_X_LO, EXTP0_PIN_X_HEAD_MAX);
		double y_pin_raw = pin_res.y_at_1;
		if (y_pin_raw < -1.1) y_pin_raw = -1.1;
		if (y_pin_raw >  1.1) y_pin_raw =  1.1;
		if (!a->c_y_pin_valid)
		{
			a->c_y_pin_try   = y_pin_raw;
			a->c_pin_cycle   = 1;
		} else
		{
			double eff_alpha;
			if (a->c_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES)
				eff_alpha = EXTP0_PIN_WARMUP_ALPHA;
			else
				eff_alpha = (pin_res.confidence == EXTRAP_CONFIDENT)
					? EXTP0_PIN_ALPHA : EXTP0_PIN_ALPHA * 0.5;
			a->c_y_pin_try = eff_alpha * y_pin_raw
				+ (1.0 - eff_alpha) * a->c_y_pin_ema;
			if (a->c_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES) a->c_pin_cycle++;
		}
	}

	nf_default_config(a->c_config);
	a->c_config->degree              = PS_NF_DEGREE;
	a->c_config->n_ctrl              = PS_NF_N_CTRL;
	a->c_config->n_ctrl_max          = PS_NF_CTRL_MAX;
	a->c_config->ordering_mode       = PS_NF_ORDERING_MODE;
	a->c_config->spearman_threshold  = PS_NF_SPEARMAN_THRESH;
	a->c_config->pre_filter_x_min    = PS_NF_PHS_PREFILT_XMIN;
	a->c_config->pre_filter_y_max    = PS_NF_PHS_PREFILT_YMAX;
	a->c_config->uniform_knots       = PS_NF_UNIFORM_KNOTS;
	a->c_config->pin_start           = PS_NF_PIN_START;
	a->c_config->start_pt            = (NF_Point2){ PS_NF_PIN_START_X, a->c_y_pin_try };
	a->c_config->pin_end             = PS_NF_PHS_PIN_END;
	a->c_config->x_weight_x0         = PS_NF_XWEIGHT_X0;
	a->c_config->x_weight_min        = PS_NF_XWEIGHT_MIN;
	a->c_config->outlier_iters       = PS_NF_OUTLIER_ITERS;
	a->c_config->outlier_sigma       = PS_NF_OUTLIER_SIGMA;
	a->c_config->outlier_min_fraction= PS_NF_OUTLIER_MIN_FRAC;
	a->c_config->cv_fraction         = PS_NF_CV_FRACTION;
	a->c_config->cv_overfit_ratio    = PS_NF_CV_OVERFIT_RATIO;
	a->c_config->cv_fatal_ratio      = PS_NF_CV_FATAL_RATIO;
	a->c_config->local_outlier_iters = PS_NF_LOCAL_OUTLIER_ITERS;
	a->c_config->local_outlier_sigma = PS_NF_LOCAL_OUTLIER_SIGMA;
	a->c_config->local_outlier_bands = PS_NF_LOCAL_OUTLIER_BANDS;
	a->c_config->fold_detect         = PS_NF_FOLD_DETECT;
	a->c_config->adaptive_iters      = PS_NF_ADAPTIVE_ITERS;
	a->c_config->adaptive_threshold  = PS_NF_ADAPTIVE_THRESH;
	a->c_config->reparam_iters       = PS_NF_REPARAM_ITERS;
	a->c_config->min_pts_per_ctrl    = PS_NF_MIN_PTS_PER_CTRL;
	a->c_config->irls_iters          = PS_NF_IRLS_ITERS;
	a->c_config->irls_epsilon        = PS_NF_IRLS_EPSILON;
	a->c_config->y_min               = PS_NF_PHS_YMIN;
	a->c_config->y_max               = PS_NF_PHS_YMAX;
	a->c_ctrl_n = a->c_config->n_ctrl;

	if (eq_used) a->c_nurb = nf_fit(a->c_nf_ws, a->c_eqd, a->eq_n, a->c_config, a->c_nfres);
	else         a->c_nurb = nf_fit(a->c_nf_ws, a->c_data, a->nsamps, a->c_config, a->c_nfres);

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
	

	for (int k = 0; k < a->nsamps; k++)
	{
		a->s_data[k].x = a->x[k];
		a->s_data[k].y = a->ys[k];
	}
	{
		ExtrapolationResult pin_res = extrapolate_y_at_0 (a->s_extrap0,
			a->x, a->ys, a->nsamps, EXTP0_PIN_X_LO, EXTP0_PIN_X_HEAD_MAX);
		double y_pin_raw = pin_res.y_at_1;
		if (y_pin_raw < -1.1) y_pin_raw = -1.1;
		if (y_pin_raw >  1.1) y_pin_raw =  1.1;
		if (!a->s_y_pin_valid)
		{
			a->s_y_pin_try   = y_pin_raw;
			a->s_pin_cycle   = 1;
		} else
		{
			double eff_alpha;
			if (a->s_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES)
				eff_alpha = EXTP0_PIN_WARMUP_ALPHA;
			else
				eff_alpha = (pin_res.confidence == EXTRAP_CONFIDENT)
					? EXTP0_PIN_ALPHA : EXTP0_PIN_ALPHA * 0.5;
			a->s_y_pin_try = eff_alpha * y_pin_raw
				+ (1.0 - eff_alpha) * a->s_y_pin_ema;
			if (a->s_pin_cycle <= EXTP0_PIN_WARMUP_CYCLES) a->s_pin_cycle++;
		}
	}

	nf_default_config(a->s_config);
	a->s_config->degree              = PS_NF_DEGREE;
	a->s_config->n_ctrl              = PS_NF_N_CTRL;
	a->s_config->n_ctrl_max          = PS_NF_CTRL_MAX;
	a->s_config->ordering_mode       = PS_NF_ORDERING_MODE;
	a->s_config->spearman_threshold  = PS_NF_SPEARMAN_THRESH;
	a->s_config->pre_filter_x_min    = PS_NF_PHS_PREFILT_XMIN;
	a->s_config->pre_filter_y_max    = PS_NF_PHS_PREFILT_YMAX;
	a->s_config->uniform_knots       = PS_NF_UNIFORM_KNOTS;
	a->s_config->pin_start           = PS_NF_PIN_START;
	a->s_config->start_pt            = (NF_Point2){ PS_NF_PIN_START_X, a->s_y_pin_try };
	a->s_config->pin_end             = PS_NF_PHS_PIN_END;
	a->s_config->x_weight_x0         = PS_NF_XWEIGHT_X0;
	a->s_config->x_weight_min        = PS_NF_XWEIGHT_MIN;
	a->s_config->outlier_iters       = PS_NF_OUTLIER_ITERS;
	a->s_config->outlier_sigma       = PS_NF_OUTLIER_SIGMA;
	a->s_config->outlier_min_fraction= PS_NF_OUTLIER_MIN_FRAC;
	a->s_config->cv_fraction         = PS_NF_CV_FRACTION;
	a->s_config->cv_overfit_ratio    = PS_NF_CV_OVERFIT_RATIO;
	a->s_config->cv_fatal_ratio      = PS_NF_CV_FATAL_RATIO;
	a->s_config->local_outlier_iters = PS_NF_LOCAL_OUTLIER_ITERS;
	a->s_config->local_outlier_sigma = PS_NF_LOCAL_OUTLIER_SIGMA;
	a->s_config->local_outlier_bands = PS_NF_LOCAL_OUTLIER_BANDS;
	a->s_config->fold_detect         = PS_NF_FOLD_DETECT;
	a->s_config->adaptive_iters      = PS_NF_ADAPTIVE_ITERS;
	a->s_config->adaptive_threshold  = PS_NF_ADAPTIVE_THRESH;
	a->s_config->reparam_iters       = PS_NF_REPARAM_ITERS;
	a->s_config->min_pts_per_ctrl    = PS_NF_MIN_PTS_PER_CTRL;
	a->s_config->irls_iters          = PS_NF_IRLS_ITERS;
	a->s_config->irls_epsilon        = PS_NF_IRLS_EPSILON;
	a->s_config->y_min               = PS_NF_PHS_YMIN;
	a->s_config->y_max               = PS_NF_PHS_YMAX;
	a->s_ctrl_n = a->s_config->n_ctrl;

	if (eq_used) a->s_nurb = nf_fit(a->s_nf_ws, a->s_eqd, a->eq_n, a->s_config, a->s_nfres);
	else         a->s_nurb = nf_fit(a->s_nf_ws, a->s_data, a->nsamps, a->s_config, a->s_nfres);

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
		a->m_y_pin_ema   = a->m_y_pin_try;
		a->m_y_pin_valid = 1;
		a->c_y_pin_ema   = a->c_y_pin_try;
		a->c_y_pin_valid = 1;
		a->s_y_pin_ema   = a->s_y_pin_try;
		a->s_y_pin_valid = 1;
	}

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
			a->m_ctrl_n = nc;
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
			a->c_ctrl_n = nc;
			a->c_ctrl_ema_valid = 1;
		}
		else
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

	{
		a->m_spline = ns_build(a->m_ns_ws, a->m_nurb, a->m_nfres, 0);
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
		if (PS_NS_EXTEND_LEFT_MODE)
			ns_extend_left(a->m_ns_ws, a->m_spline, PS_NS_EXTEND_X_TARGET, a->m_anchor_ema,
				PS_NS_EXTEND_BOUND_FRAC, 0.1, 2.0);

		int m_extrema = count_extrema(a->m_extrema, a->m_spline);
		if (m_extrema > 2)
		{
			a->binfo[1] |= 0b00010000;
			goto cleanup;
		}
	}

	{
		a->c_spline = ns_build(a->c_ns_ws, a->c_nurb, a->c_nfres, 0);
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
		if (PS_NS_EXTEND_LEFT_MODE)
			ns_extend_left(a->c_ns_ws, a->c_spline, PS_NS_EXTEND_X_TARGET, a->c_anchor_ema,
				PS_NS_EXTEND_BOUND_FRAC, -1.1, 1.1);
	}

	{
		a->s_spline = ns_build(a->s_ns_ws, a->s_nurb, a->s_nfres, 0);
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
		if (PS_NS_EXTEND_LEFT_MODE)
			ns_extend_left(a->s_ns_ws, a->s_spline, PS_NS_EXTEND_X_TARGET, a->s_anchor_ema,
				PS_NS_EXTEND_BOUND_FRAC, -1.1, 1.1);
	}

	if (sin_cos_identity_check(a))
	{
		a->binfo[2] |= 0b00010000;
		a->binfo[3] |= 0b00010000;
		goto cleanup;
	}

	if (scheck(a))
	{
		a->binfo[6] |= 0b0001;
		goto cleanup;
	}

	curve_ema_update(&a->m_calavg, a->m_spline);
	a->m_prev_y = a->m_calavg.ys[0];
	curve_ema_update(&a->c_calavg, a->c_spline);
	a->c_prev_y = a->c_calavg.ys[0];
	curve_ema_update(&a->s_calavg, a->s_spline);
	a->s_prev_y = a->s_calavg.ys[0];

	EnterCriticalSection (&a->disp.cs_disp);
	a->disp.nsamps = a->nsamps;
	memcpy(a->disp.x,  a->x,  a->nsamps * sizeof (double));
	memcpy(a->disp.ym, a->ym, a->nsamps * sizeof (double));
	memcpy(a->disp.yc, a->yc, a->nsamps * sizeof (double));
	memcpy(a->disp.ys, a->ys, a->nsamps * sizeof (double));

	a->disp.m_prev_y = a->m_calavg.ys[0];
	for (int k = 0; k < DISP_PTS; k++)
	{
		double cx = (double)k / (double)(DISP_PTS - 1);
		double y = get_mag_correction_ema(&a->m_calavg, cx, &a->disp.m_prev_y);
		a->disp.xm_cor[k] = cx;
		a->disp.ym_cor[k] = y;
	}
	a->disp.ym_cor[0] = a->disp.ym_cor[1];
	a->disp.c_prev_y = a->c_calavg.ys[0];
	for (int k = 0; k < DISP_PTS; k++)
	{
		a->disp.xc_cor[k] = (double)k / (double)(DISP_PTS - 1);
		a->disp.yc_cor[k] = get_phase_correction_ema(&a->c_calavg,
			a->disp.xc_cor[k], &a->disp.c_prev_y);
	}
	a->disp.s_prev_y = a->s_calavg.ys[0];
	for (int k = 0; k < DISP_PTS; k++)
	{
		a->disp.xs_cor[k] = (double)k / (double)(DISP_PTS - 1);
		a->disp.ys_cor[k] = get_phase_correction_ema(&a->s_calavg,
			a->disp.xs_cor[k], &a->disp.s_prev_y);
	}

	const double rad2deg = 180.0 / PI;
	a->disp.xa_cor[0] = 0.0;
	double sinval, cosval;
	sinval = a->disp.ys_cor[0];
	cosval = a->disp.yc_cor[0];
	a->disp.ya_cor[0] = rad2deg * atan2(sinval, cosval);
	for (int k = 1; k < DISP_PTS; k++)
	{
		a->disp.xa_cor[k] = (double)k / (double)(DISP_PTS - 1);
		sinval = a->disp.ys_cor[k];
		cosval = a->disp.yc_cor[k];
		double raw = rad2deg * atan2(sinval, cosval);
		double delta = raw - a->disp.ya_cor[k - 1];
		while (delta >  180.0) delta -= 360.0;
		while (delta < -180.0) delta += 360.0;
		a->disp.ya_cor[k] = a->disp.ya_cor[k - 1] + delta;
	}
	a->disp.phs_ref_deg = a->disp.ya_cor[DISP_PTS - 1];
	double target_center_deg = 0.0;
	double angle_offset = target_center_deg - a->disp.ya_cor[DISP_PTS - 1];
	for (int k = 0; k < DISP_PTS; k++)
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
				curve_ema_init2(&a->m_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND,  0.1, 2.0);
				curve_ema_init2(&a->c_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND, -1.1, 1.1);
				curve_ema_init2(&a->s_calavg, PS_NS_EMA_ALPHA, PS_NS_EMA_ALPHA_LO, PS_NS_EMA_X_BND, -1.1, 1.1);
				a->m_fold_prev      = 0;
				a->m_ctrl_ema_valid = 0;
				a->c_ctrl_ema_valid = 0;
				a->s_ctrl_ema_valid = 0;
				a->m_y_pin_valid = 0; a->m_y_pin_ema = 1.0; a->m_pin_cycle = 0;
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
				sampleCollectClear(a->ps_colct);
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
					putSample(&tx[2 * i], &rx[2 * i], a->hw_scale, a->ps_colct);
					full = sampleCheckAndUpdate(a->ps_colct);
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
					a->ps_colct->bbtm[SAMPLE_NBUCKS - 1]) < DEADLOCK_MIN_FRAC)
				{
					a->ctrl.state = LRESET;
					a->info[6] |= 2;
				}
				else if (a->ctrl.count >= 5 * a->rate)
				{
					a->ctrl.count = 0;
					sampleCollectClear(a->ps_colct);
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
							a->ps_colct->bbtm[SAMPLE_NBUCKS - 1]) < DEADLOCK_MIN_FRAC)
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
	memcpy(xm_cor, a->disp.xm_cor, DISP_PTS * sizeof(double));
	memcpy(ym_cor, a->disp.ym_cor, DISP_PTS * sizeof(double));
	memcpy(xa_cor, a->disp.xa_cor, DISP_PTS * sizeof(double));
	memcpy(ya_cor, a->disp.ya_cor, DISP_PTS * sizeof(double));
	*nsamps_out      = disp_nsamps;
	*cpts_out        = DISP_PTS;
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

			if (b && c && d)
			{
				double m_max_err, m_rms_err;
				ns_accuracy_check(c, d, 1000, &m_max_err, &m_rms_err);
				double m_noise_est = fmax(b->rms, b->cv_score);
				fprintf(file, "Spline max_err=%.2e is %.0fx noise (%.2e)\n\n",
					m_max_err, m_max_err / m_noise_est, m_noise_est);
			}

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
				double pin_val    = (rtype==1) ? a->m_y_pin_ema :
				                    (rtype==2) ? a->c_y_pin_ema : a->s_y_pin_ema;
				int    pin_valid  = (rtype==1) ? a->m_y_pin_valid :
				                    (rtype==2) ? a->c_y_pin_valid : a->s_y_pin_valid;
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
			for (int k = 0; k < DISP_PTS; k++)
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
		for (int i = 0, k = 0; i < SAMPLE_NBUCKS; i++)
		{
			for (int j = 0; j < a->ps_colct->tpb[i]; j++)
			{
				fprintf(f, "%5d    %12.4e   %12.4e        %12.4e   %12.4e   %12.4e   %12.4e\n", i,
					a->ps_colct->smps[k].envTX, a->ps_colct->smps[k].envRX,
					a->x[k], a->ym[k], a->yc[k], a->ys[k]);
				k++;
			}
		}
		fflush(f);
		fclose(f);
	}
}

void print_EQ_Samples(CALCC a)
{
	FILE* f = NULL;
	char* filename = "EQ_Samples.txt";
	f = fopen(filename, "w");
	if (f)
	{
		for (int i = 0; i < a->eq_n; i++)
		{
			fprintf(f, "%.6e      %.6e      %.6e      %.6e\n",
				a->m_eqd[i].x, a->m_eqd[i].y, a->c_eqd[i].y, a->s_eqd[i].y);
		}
		fflush(f);
		fclose(f);
	}
}

