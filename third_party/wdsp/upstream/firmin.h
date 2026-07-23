/*  firmin.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2016 Warren Pratt, NR0V

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

/********************************************************************************************************
*																										*
*											Time-Domain FIR												*
*																										*
********************************************************************************************************/

#ifndef _firmin_h
#define _firmin_h

typedef struct _firmin
{
	int run;				// run control
	int position;			// position at which to execute
	int size;				// input/output buffer size, power of two
	double* in;				// input buffer
	double* out;			// output buffer, can be same as input
	int nc;					// number of filter coefficients, power of two
	double f_low;			// low cutoff frequency
	double f_high;			// high cutoff frequency
	double* ring;			// internal complex ring buffer
	double* h;				// complex filter coefficients
	int rsize;				// ring size, number of complex samples, power of two
	int mask;				// mask to update indexes
	int idx;				// ring input/output index
	double samplerate;		// sample rate
	int wintype;			// filter window type
	double gain;			// filter gain
}firmin, *FIRMIN;

extern FIRMIN create_firmin (int run, int position, int size, double* in, double* out, 
	int nc, double f_low, double f_high, int samplerate, int wintype, double gain);

extern void destroy_firmin (FIRMIN a);

extern void flush_firmin (FIRMIN a);

extern void xfirmin (FIRMIN a, int pos);

extern void setBuffers_firmin (FIRMIN a, double* in, double* out);

extern void setSamplerate_firmin (FIRMIN a, int rate);

extern void setSize_firmin (FIRMIN a, int size);

extern void setFreqs_firmin (FIRMIN a, double f_low, double f_high);

#endif

/********************************************************************************************************
*																										*
*								Standalone Partitioned Overlap-Save Bandpass							*
*																										*
********************************************************************************************************/

#ifndef _firopt_h
#define _firopt_h

typedef struct _firopt
{
	int run;				// run control
	int position;			// position at which to execute
	int size;				// input/output buffer size, power of two
	double* in;				// input buffer
	double* out;			// output buffer, can be same as input
	int nc;					// number of filter coefficients, power of two, >= size
	double f_low;			// low cutoff frequency
	double f_high;			// high cutoff frequency
	double samplerate;		// sample rate
	int wintype;			// filter window type
	double gain;			// filter gain
	int nfor;				// number of buffers in delay line
	double* fftin;			// fft input buffer
	double** fmask;			// frequency domain masks
	double** fftout;		// fftout delay line
	double* accum;			// frequency domain accumulator
	int buffidx;			// fft out buffer index
	int idxmask;			// mask for index computations
	double* maskgen;		// input for mask generation FFT
	fftw_plan* pcfor;		// array of forward FFT plans
	fftw_plan crev;			// reverse fft plan
	fftw_plan* maskplan;	// plans for frequency domain masks
} firopt, *FIROPT;

extern FIROPT create_firopt (int run, int position, int size, double* in, double* out, 
	int nc, double f_low, double f_high, int samplerate, int wintype, double gain);

extern void xfiropt (FIROPT a, int pos);

extern void destroy_firopt (FIROPT a);

extern void flush_firopt (FIROPT a);

extern void setBuffers_firopt (FIROPT a, double* in, double* out);

extern void setSamplerate_firopt (FIROPT a, int rate);

extern void setSize_firopt (FIROPT a, int size);

extern void setFreqs_firopt (FIROPT a, double f_low, double f_high);

#endif

/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Filter Kernel								*
*																										*
********************************************************************************************************/

#ifndef _fircore_h
#define _fircore_h

#include "fir.h"

typedef struct _fircore
{
	int size;				// input/output buffer size, power of two
	double* in;				// input buffer
	double* out;			// output buffer, can be same as input
	int nc;					// number of filter coefficients, power of two, >= size
	double* impulse;		// impulse response of filter
	double* imp;
	int nfor;				// number of buffers in delay line
	double* fftin;			// fft input buffer
	double*** fmask;		// frequency domain masks
	double** fftout;		// fftout delay line
	double* accum;			// frequency domain accumulator
	int buffidx;			// fft out buffer index
	int idxmask;			// mask for index computations
	double* maskgen;		// input for mask generation FFT
	fftw_plan* pcfor;		// array of forward FFT plans
	fftw_plan crev;			// reverse fft plan
	fftw_plan** maskplan;	// plans for frequency domain masks
	CRITICAL_SECTION update;
	int cset;
	int mp;
	int masks_ready;
	int pfactor;
	MINPHASE pminphase;
} fircore, *FIRCORE;

extern FIRCORE create_fircore (int size, double* in, double* out, 
	int nc, int mp, int pfactor, double* impulse);

extern void xfircore (FIRCORE a);

extern void destroy_fircore (FIRCORE a);

extern void flush_fircore (FIRCORE a);

extern void setBuffers_fircore (FIRCORE a, double* in, double* out);

extern void setSize_fircore (FIRCORE a, int size);

extern void setImpulse_fircore (FIRCORE a, double* impulse, int update);

extern void setNc_fircore (FIRCORE a, int nc, double* impulse);

extern void setMp_fircore (FIRCORE a, int mp);

extern void setUpdate_fircore (FIRCORE a);

#endif
