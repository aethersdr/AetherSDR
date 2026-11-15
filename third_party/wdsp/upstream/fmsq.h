/*  fmsq.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2026 Warren Pratt, NR0V

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

#ifndef _fmsq_h
#define _fmsq_h
#include "eq.h"
#include "fir.h"
#include "firmin.h"
typedef struct _fmsq
{
	int run;							// 0 if squelch system is OFF; 1 if it's ON
	int size;							// size of input/output buffers
	double* insig;						// squelch input signal buffer
	double* outsig;						// squelch output signal buffer
	double* trigger;					// buffer used to trigger mute/unmute (may be same as input; matches timing of input buffer)
	double rate;						// sample rate
	double* noise;
	double fc;							// corner frequency for sig / noise detection
	double* pllpole;					// pointer to pole frequency of the fm demodulator pll
	double F[4];
	double G[4];
	double avtau;						// time constant for averaging noise
	double avm;						
	double onem_avm;
	double avnoise;
	double longtau;						// time constant for long averaging
	double longavm;
	double onem_longavm;
	double longnoise;
	int state;							// state machine control
	int count;
	double tup;
	double tdown;
	int ntup;
	int ntdown;
	double* cup;
	double* cdown;
	double tail_thresh;
	double unmute_thresh;
	double min_tail;
	double max_tail;
	int ready;
	double ramp;
	double rstep;
	double tdelay;
	int nc;
	int mp;
	FIRCORE p;
	EQIMP peqimp;
	double* impulse;
} fmsq, *FMSQ;

extern FMSQ create_fmsq (int run, int size, double* insig, double* outsig, double* trigger, int rate, double fc, 
	double* pllpole, double tdelay, double avtau, double longtau, double tup, double tdown, double tail_thresh, 
	double unmute_thresh, double min_tail, double max_tail, int nc, int mp);

extern void destroy_fmsq (FMSQ a);

extern void flush_fmsq (FMSQ a);

extern void xfmsq (FMSQ a);

extern void setBuffers_fmsq (FMSQ a, double* in, double* out, double* trig);

extern void setSamplerate_fmsq (FMSQ a, int rate);

extern void setSize_fmsq (FMSQ a, int size);

// RXA Properties

extern __declspec (dllexport) void SetRXAFMSQThreshold (int channel, double threshold);

extern __declspec (dllexport) void SetRXAFMSQNC (int channel, int nc);

extern __declspec (dllexport) void SetRXAFMSQMP (int channel, int mp);

#endif
