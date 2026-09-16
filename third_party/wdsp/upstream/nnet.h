/*  nnet.h

Neural noise reduction - network layers and model assembly

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

#ifndef _nnet_h
#define _nnet_h

#include "nnio.h"

typedef struct _nnet* NNET;

#define NNET_MODE_NETWORK	0
#define NNET_MODE_IDENTITY	1
#define NNET_MODE_LOWPASS	2

#ifndef NNET_NSLOTS
#define NNET_NSLOTS 2
#endif

extern NNET create_nnet (int nbins, int lookahead, double floor_db);

extern void destroy_nnet (NNET n);

extern void flush_nnet (NNET n);

extern void run_nnet
	(
	NNET n,
	const double* xre,
	const double* xim,
	double* yre,
	double* yim
	);

extern void setFloor_nnet (NNET n, double floor_db);

extern void runCore_nnet (NNET n, const double* x);

extern const double* getStage_nnet (NNET n, const char* name,
	int* nch, int* nfreq);

extern int isReady_nnet (NNET n);

extern __declspec (dllexport) void SetNNRModelPath (const char* path);

// diagnostics
extern int getBins_nnet (NNET n);

extern int getLookahead_nnet (NNET n);

extern void setMode_nnet (NNET n, int mode);

extern int getMode_nnet (NNET n);


extern void setAlpha_nnet (NNET n, double alpha);

extern double getAlpha_nnet (NNET n);

extern void setKnee_nnet (NNET n, double knee_db);

extern double getKnee_nnet (NNET n);

extern void setTau_nnet (NNET n, double tau);

extern double getTau_nnet (NNET n);

extern void setMaxGain_nnet (NNET n, double gmax_db);

extern double getMaxGain_nnet (NNET n);

extern void setSmooth_nnet (NNET n, double att_ms, double rel_ms);

extern void getSmooth_nnet (NNET n, double* att_ms, double* rel_ms);


typedef struct _gru* GRU;

extern GRU create_gru
	(
	int nin,
	int nhidden,
	const double* w_ih,
	const double* w_hh,
	const double* b_ih,
	const double* b_hh
	);

extern void destroy_gru (GRU g);

extern void flush_gru (GRU g);

extern const double* run_gru (GRU g, const double* x);

extern const double* run_gru_dbg
	(
	GRU g,
	const double* x,
	double* rout,
	double* zout,
	double* nout
	);

extern const double* getState_gru (GRU g);

extern void setState_gru (GRU g, const double* h);

typedef struct _conv2d* CONV2D;

extern CONV2D create_conv2d
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
	);

extern void destroy_conv2d (CONV2D c);

extern void flush_conv2d (CONV2D c);

extern const double* run_conv2d (CONV2D c, const double* x);

extern int getFout_conv2d (CONV2D c);

extern int getCout_conv2d (CONV2D c);

extern void run_prelu (double* x, const double* slope, int nch, int nfreq);

typedef struct _convt2d* CONVT2D;

extern CONVT2D create_convt2d
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
	);

extern void destroy_convt2d (CONVT2D c);

extern void flush_convt2d (CONVT2D c);

extern const double* run_convt2d (CONVT2D c, const double* x);

extern int getFout_convt2d (CONVT2D c);

extern int getCout_convt2d (CONVT2D c);

typedef struct _dfhead* DFHEAD;

extern DFHEAD create_dfhead
	(
	int nbins,
	int order,
	double gmin_db,
	double gmax_db
	);

extern void destroy_dfhead (DFHEAD d);

extern void flush_dfhead (DFHEAD d);

extern void run_dfhead
	(
	DFHEAD d,
	const double* xre,
	const double* xim,
	const double* coef,
	double* yre,
	double* yim
	);

extern void setGains_dfhead (DFHEAD d, double gmin_db, double gmax_db);

extern void setAlpha_dfhead (DFHEAD d, double alpha);

extern double getAlpha_dfhead (DFHEAD d);

extern void setKnee_dfhead (DFHEAD d, double knee_db);

extern double getKnee_dfhead (DFHEAD d);

extern void setSmooth_dfhead (DFHEAD d, double att_ms, double rel_ms,
	double frame_rate);

extern int getOrder_dfhead (DFHEAD d);

typedef struct _cond* COND;

extern COND create_cond
	(
	int nbins,
	double cexp,
	double tau,
	double frame_rate,
	double pfloor
	);

extern void destroy_cond (COND c);

extern void flush_cond (COND c);

extern const double* run_cond (COND c, const double* xre, const double* xim);

extern double getGain_cond (COND c);

extern void setTau_cond (COND c, double tau);

typedef struct _dprnn* DPRNN;

extern DPRNN create_dprnn
	(
	NNIO f,
	const char* prefix,
	int nch,
	int nfreq,
	int hid
	);

extern void destroy_dprnn (DPRNN d);

extern void flush_dprnn (DPRNN d);

extern const double* run_dprnn (DPRNN d, const double* x);

extern int ok_dprnn (DPRNN d);

#ifdef NNET_PROFILE
extern void report_profile_nnet (void);
extern void reset_profile_nnet (void);
#endif

extern int ok_nnet (NNET n);

extern NNET create_nnet_slot (int slot, int nbins, int lookahead,
                              double floor_db);

#endif
