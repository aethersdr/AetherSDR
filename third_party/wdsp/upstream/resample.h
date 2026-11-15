/*  resample.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013 Warren Pratt, NR0V

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

/************************************************************************************************
*																								*
*							  VERSION FOR COMPLEX DOUBLE-PRECISION								*
*																								*
************************************************************************************************/

#ifndef _resample_h
#define _resample_h

typedef struct _resample
{
	int run;			// run
	int size;			// number of input samples per buffer
	double* in;			// input buffer for resampler
	double* out;		// output buffer for resampler
	int in_rate;
	int out_rate;
	double fcin;
	double fc;
	double fc_low;
	double gain;
	int idx_in;			// index for input into ring
	int ncoefin;
	int ncoef;			// number of coefficients
	int L;				// interpolation factor
	int M;				// decimation factor
	double* h;			// coefficients
	int ringsize;		// number of complex pairs the ring buffer holds
	double* ring;		// ring buffer
	int cpp;			// coefficients of the phase
	int phnum;			// phase number
} resample, *RESAMPLE;

__declspec (dllexport)
RESAMPLE create_resample (int run, int size, double* in, double* out, int in_rate, int out_rate, double fc, int ncoef, double gain);

__declspec (dllexport)
void destroy_resample (RESAMPLE a);

__declspec (dllexport)
void flush_resample (RESAMPLE a);

__declspec (dllexport)
int xresample (RESAMPLE a);

extern void setBuffers_resample (RESAMPLE a, double* in, double* out);

extern void setSize_resample(RESAMPLE a, int size);

extern void setInRate_resample(RESAMPLE a, int rate);

extern void setOutRate_resample(RESAMPLE a, int rate);

extern void setFCLow_resample (RESAMPLE a, double fc_low);

extern void setBandwidth_resample (RESAMPLE a, double fc_low, double fc_high);

#endif

/************************************************************************************************
*																								*
*							  VERSION FOR NON-COMPLEX FLOATS									*
*																								*
************************************************************************************************/

#ifndef _resampleF_h
#define _resampleF_h

typedef struct _resampleF
{
	int run;			// run
	int size;			// number of input samples per buffer
	float* in;			// input buffer for resampler
	float* out;			// output buffer for resampler
	int idx_in;			// index for input into ring
	int ncoef;			// number of coefficients
	int L;				// interpolation factor
	int M;				// decimation factor
	double* h;			// coefficients
	int ringsize;		// number of values the ring buffer holds
	double* ring;		// ring buffer
	int cpp;			// coefficients of the phase
	int phnum;			// phase number
} resampleF, *RESAMPLEF;

extern RESAMPLEF create_resampleF (int run, int size, float* in, float* out, int in_rate, int out_rate);

extern void destroy_resampleF (RESAMPLEF a);

extern void flush_resampleF (RESAMPLEF a);

extern int xresampleF (RESAMPLEF a);

#endif
