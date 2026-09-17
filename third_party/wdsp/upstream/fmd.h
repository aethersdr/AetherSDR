/*  fmd.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2026 Warren Pratt, NR0V

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

#ifndef _fmd_h
#define _fmd_h

typedef struct _fmd* FMD;

extern FMD create_fmd ( int run, int size, double* in, double* out, int rate, double deviation, 
	double f_low, double f_high, double fmin, double fmax, double zeta, double omegaN, double tau, 
	double afgain, int sntch_run, double ctcss_freq, int nc_de, int mp_de, int nc_aud, int mp_aud);

extern void destroy_fmd (FMD a);

extern void flush_fmd (FMD a);

extern void xfmd (FMD a);

extern void setBuffers_fmd (FMD a, double* in, double* out);

extern void setSamplerate_fmd (FMD a, int rate);

extern void setSize_fmd (FMD a, int size);

extern double* getFMDpAudio (int channel);

extern double* getFMDpPllpole (int channel);

extern void setFMDRun (int channel, int run);

// RXA Properties

extern __declspec (dllexport) void SetRXAFMDeviation (int channel, double deviation);

extern __declspec (dllexport) void SetRXAFMNCde (int channel, int nc);

extern __declspec (dllexport) void SetRXAFMMPde (int channel, int mp);

extern __declspec (dllexport) void SetRXAFMNCaud (int channel, int nc);

extern __declspec (dllexport) void SetRXAFMMPaud (int channel, int mp);

#endif