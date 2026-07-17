/*  anr.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2012, 2013 Warren Pratt, NR0V

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

#ifndef _anr_h
#define _anr_h

#define ANR_DLINE_SIZE 2048

typedef struct _anr
{
	int run;
	int position;
	int buff_size;
	double *in_buff;
	double *out_buff;
	int dline_size;
	int mask;
	int n_taps;
	int delay;
	double two_mu;
	double gamma;
	double d [ANR_DLINE_SIZE];
	double w [ANR_DLINE_SIZE];
	int in_idx;

	double lidx;
	double lidx_min;
	double lidx_max;
	double ngamma;
	double den_mult;
	double lincr;
	double ldecr;
} anr, *ANR;

extern ANR create_anr	(
				int run,
				int position,
				int buff_size,
				double *in_buff,
				double *out_buff,
				int dline_size,
				int n_taps,
				int delay,
				double two_mu,
				double gamma,

				double lidx,
				double lidx_min,
				double lidx_max,
				double ngamma,
				double den_mult,
				double lincr,
				double ldecr
			);

extern void destroy_anr (ANR a);

extern void flush_anr (ANR a);

extern void xanr (ANR a, int position);

extern void setBuffers_anr (ANR a, double* in, double* out);

extern void setSamplerate_anr (ANR a, int rate);

extern void setSize_anr (ANR a, int size);

// RXA Properties

extern __declspec (dllexport) void SetRXAANRRun (int channel, int setit);

extern __declspec (dllexport) void SetRXAANRVals (int channel, int taps, int delay, double gain, double leakage);

extern __declspec (dllexport) void SetRXAANRTaps (int channel, int taps);

extern __declspec (dllexport) void SetRXAANRDelay (int channel, int delay);

extern __declspec (dllexport) void SetRXAANRGain (int channel, double gain);

extern __declspec (dllexport) void SetRXAANRLeakage (int channel, double leakage);

extern __declspec (dllexport) void SetRXAANRPosition (int channel, int position);

#endif