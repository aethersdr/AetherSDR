/*  fmmod.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2023 Warren Pratt, NR0V

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

#ifndef _fmmod_h
#define _fmmod_h
#include "firmin.h"
typedef struct _fmmod
{
	int run;
	int size;
	double* in;
	double* out;
	double samplerate;
	double deviation;
	double f_low;
	double f_high;
	int ctcss_run;
	double ctcss_level;
	double ctcss_freq;
	// for ctcss gen
	double tscale;
	double tphase;
	double tdelta;
	// mod
	double sphase;
	double sdelta;
	// bandpass
	int bp_run;
	double bp_fc;
	int nc;
	int mp;
	FIRCORE p;
}fmmod, *FMMOD;

extern FMMOD create_fmmod (int run, int size, double* in, double* out, int rate, double dev, double f_low, double f_high, 
	int ctcss_run, double ctcss_level, double ctcss_freq, int bp_run, int nc, int mp);

extern void destroy_fmmod (FMMOD a);

extern void flush_fmmod (FMMOD a);

extern void xfmmod (FMMOD a);

extern void setBuffers_fmmod (FMMOD a, double* in, double* out);

extern void setSamplerate_fmmod (FMMOD a, int rate);

extern void setSize_fmmod (FMMOD a, int size);

// TXA Properties

extern __declspec (dllexport) void SetTXAFMDeviation (int channel, double deviation);

extern __declspec (dllexport) void SetTXACTCSSFreq (int channel, double freq);

extern __declspec (dllexport) void SetTXACTCSSRun (int channel, int run);

extern __declspec (dllexport) void SetTXAFMMP (int channel, int mp);

extern __declspec (dllexport) void SetTXAFMNC (int channel, int nc);

extern __declspec (dllexport) void SetTXAFMAFFreqs (int channel, double low, double high);

#endif