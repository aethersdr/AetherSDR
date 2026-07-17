/*  emnr.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2015 Warren Pratt, NR0V

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

#ifndef _emnr_h
#define _emnr_h

typedef struct _emnr
{
	int run;
	int position;
	int bsize;
	double* in;
	double* out;
	int fsize;
	int ovrlp;
	int incr;
	double* window;
	int iasize;
	double* inaccum;
	double* forfftin;
	double* forfftout;
	int msize;
	double* mask;
	double* revfftin;
	double* revfftout;
	double** save;
	int oasize;
	double* outaccum;
	double rate;
	int wintype;
	double ogain;
	double gain;
	int nsamps;
	int iainidx;
	int iaoutidx;
	int init_oainidx;
	int oainidx;
	int oaoutidx;
	int saveidx;
	fftw_plan Rfor;
	fftw_plan Rrev;
	struct _g
	{
		int gain_method;
		int npe_method;
		int ae_run;
		double msize;
		double* mask;
		double* y;
		double* lambda_y;
		double* lambda_d;
		double* prev_mask;
		double* prev_gamma;
		double gf1p5;
		double alpha;
		double eps_floor;
		double gamma_max;
		double xi_min;
		double q;
		double gmax;
		//
		double* GG;
		double* GGS;
		FILE* fileb;
		//
		int dim_zeta;
		double* zeta_hat;
		int* zeta_true;
		double z_gamma_min;
		double z_gamma_max;
		double z_xihat_min;
		double z_xihat_max;
		double zeta_thresh;
	} g;
	struct _npest
	{
		int incr;
		double rate;
		int msize;
		double* lambda_y;
		double* lambda_d;
		double* p;
		double* alphaOptHat;
		double alphaC;
		double alphaCsmooth;
		double alphaCmin;
		double* alphaHat;
		double alphaMax;
		double* sigma2N;
		double alphaMin_max_value;
		double snrq;
		double betamax;
		double* pbar;
		double* p2bar;
		double invQeqMax;
		double av;
		double* Qeq;
		int U;
		double Dtime;
		int V;
		int D;
		double MofD;
		double MofV;
		double* bmin;
		double* bmin_sub;
		int* k_mod;
		double* actmin;
		double* actmin_sub;
		int subwc;
		int* lmin_flag;
		double* pmin_u;
		double invQbar_points[4];
		double nsmax[4];
		double** actminbuff;
		int amb_idx;
	} np;
	struct _npests
	{
		int incr;
		double rate;
		int msize;
		double* lambda_y;
		double* lambda_d;
		
		double alpha_pow;
		double alpha_Pbar;
		double epsH1;
		double epsH1r;

		double* sigma2N;
		double* PH1y;
		double* Pbar;
		double* EN2y;
	} nps;
	struct _npestl
	{
		double rate;
		int msize;
		int incr;
		double* Ysq;
		double* P;
		double* Pmin;
		double* p;
		double* D;
		double* lambda_d;

		double eta;
		double gamma;
		double beta;
		double delta_LF;
		double delta_MF;
		double delta_0;
		double delta_1;
		double delta_2;
		double alpha_d;
		double alpha_p;
	} npl;
	struct _ae
	{
		int msize;
		double* lambda_y;
		double zetaThresh;
		double psi;
		double* nmask;
		double t2;
	} ae;
	struct _post2
	{
		int run;
		double nlevel;
		double factor;
		double taper;
		double tc_decay;
		double rate_decay;
		double* w;
		int noise_frames;
		int noise_frame_index;
		double* noise_frame;
		double olddmag;
	} post2;
}emnr, *EMNR;

extern EMNR create_emnr (int run, int position, int size, double* in, double* out, int fsize, int ovrlp, 
	int rate, int wintype, double gain, int gain_method, int npe_method, int ae_run);

extern void destroy_emnr (EMNR a);

extern void flush_emnr (EMNR a);

extern void xemnr (EMNR a, int pos);

extern void setBuffers_emnr (EMNR a, double* in, double* out);

extern void setSamplerate_emnr (EMNR a, int rate);

extern void setSize_emnr (EMNR a, int size);

#endif