/*  wcpAGC.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2011, 2012, 2013 Warren Pratt, NR0V

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

#ifndef _wcpagc_h
#define _wcpagc_h

#define MAX_SAMPLE_RATE		(384000.0)
#define MAX_N_TAU			(8)
#define MAX_TAU_ATTACK		(0.01)
#define RB_SIZE				(int)(MAX_SAMPLE_RATE * MAX_N_TAU * MAX_TAU_ATTACK + 1)

#define AGCPORT				__declspec(dllexport)

typedef struct _wcpagc
{
	int run;
	int mode;
	int pmode;
	double* in;
	double* out;
	int io_buffsize;
	double sample_rate;

	double tau_attack;
	double tau_decay;
	int n_tau;
	double max_gain;
	double var_gain;
	double fixed_gain;
	double min_volts;
	double max_input;
	double out_targ;
	double out_target;
	double inv_max_input;
	double slope_constant;

	double gain;
	double inv_out_target;

	int out_index;
	int in_index;
	int attack_buffsize;

	double* ring;
	double* abs_ring;
	int ring_buffsize;
	double ring_max;

	double attack_mult;
	double decay_mult;
	double volts;
	double save_volts;
	double out_sample[2];
	double abs_out_sample;
	int state;

	double tau_fast_backaverage;
	double fast_backmult;
	double onemfast_backmult;
	double fast_backaverage;
	double tau_fast_decay;
	double fast_decay_mult;
	double pop_ratio;

	int hang_enable;
	double hang_backaverage;
	double tau_hang_backmult;
	double hang_backmult;
	double onemhang_backmult;
	int hang_counter;
	double hangtime;
	double hang_thresh;
	double hang_level;

	double tau_hang_decay;
	double hang_decay_mult;
	int decay_type;
} wcpagc, *WCPAGC;

extern void loadWcpAGC (WCPAGC a);

extern void xwcpagc (WCPAGC a);

extern WCPAGC create_wcpagc (	int run,
								int mode,
								int pmode,
								double* in,
								double* out,
								int io_buffsize,
								int sample_rate,
								double tau_attack,
								double tau_decay,
								int n_tau,
								double max_gain,
								double var_gain,
								double fixed_gain,
								double max_input,
								double out_targ,
								double tau_fast_backaverage,
								double tau_fast_decay,
								double pop_ratio,
								int hang_enable,
								double tau_hang_backmult,
								double hangtime,
								double hang_thresh,
								double tau_hang_decay
								);

extern void destroy_wcpagc (WCPAGC a);

extern void flush_wcpagc (WCPAGC a);

extern void setBuffers_wcpagc (WCPAGC a, double* in, double* out);

extern void setSamplerate_wcpagc (WCPAGC a, int rate);

extern void setSize_wcpagc (WCPAGC a, int size);

// RXA Properties

extern AGCPORT void SetRXAAGCMode (int channel, int mode);

extern AGCPORT void SetRXAAGCFixed (int channel, double fixed_agc);

extern AGCPORT void SetRXAAGCAttack (int channel, int attack);

extern AGCPORT void SetRXAAGCDecay (int channel, int decay);

extern AGCPORT void SetRXAAGCHang (int channel, int hang);

extern AGCPORT void GetRXAAGCHangLevel(int channel, double *hangLevel);

extern AGCPORT void SetRXAAGCHangLevel(int channel, double hangLevel);

extern AGCPORT void GetRXAAGCHangThreshold(int channel, int *hangthreshold);

extern AGCPORT void SetRXAAGCHangThreshold (int channel, int hangthreshold);

extern AGCPORT void GetRXAAGCTop(int channel, double *max_agc);

extern AGCPORT void SetRXAAGCTop (int channel, double max_agc);

extern AGCPORT void SetRXAAGCSlope (int channel, int slope);

extern AGCPORT void SetRXAAGCThresh(int channel, double thresh, double size, double rate);

extern AGCPORT void GetRXAAGCThresh(int channel, double *thresh, double size, double rate);

// TXA Properties

extern AGCPORT void SetTXAALCSt (int channel, int state);

extern AGCPORT void SetTXAALCAttack (int channel, int attack);

extern AGCPORT void SetTXAALCDecay (int channel, int decay);

extern AGCPORT void SetTXAALCHang (int channel, int hang);

extern AGCPORT void SetTXALevelerSt (int channel, int state);

extern AGCPORT void SetTXALevelerAttack (int channel, int attack);

extern AGCPORT void SetTXALevelerDecay (int channel, int decay);

extern AGCPORT void SetTXALevelerHang (int channel, int hang);

extern AGCPORT void SetTXALevelerTop (int channel, double maxgain);

#endif

