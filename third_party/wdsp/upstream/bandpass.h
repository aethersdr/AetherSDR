/*  bandpass.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2017 Warren Pratt, NR0V

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
*										Overlap-Save Bandpass											*
*																										*
********************************************************************************************************/

#ifndef _bps_h
#define _bps_h

typedef struct _bps
{
	int run;
	int position;
	int size;
	double* in;
	double* out;
	double f_low;
	double f_high;
	double* infilt;
	double* product;
	double* mults;
	double samplerate;
	int wintype;
	double gain;
	fftw_plan CFor;
	fftw_plan CRev;
}bps, *BPS;

extern BPS create_bps (int run, int position, int size, double* in, double* out, 
	double f_low, double f_high, int samplerate, int wintype, double gain);

extern void destroy_bps (BPS a);

extern void flush_bps (BPS a);

extern void xbps (BPS a, int pos);

extern void setBuffers_bps (BPS a, double* in, double* out);

extern void setSamplerate_bps (BPS a, int rate);

extern void setSize_bps (BPS a, int size);

extern void setFreqs_bps (BPS a, double f_low, double f_high);

// RXA Prototypes

extern __declspec (dllexport) void SetRXABPSRun (int channel, int run);

extern __declspec (dllexport) void SetRXABPSFreqs (int channel, double low, double high);

// TXA Prototypes

extern __declspec (dllexport) void SetTXABPSRun (int channel, int run);

extern __declspec (dllexport) void SetTXABPSFreqs (int channel, double low, double high);

#endif


/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Bandpass									*
*																										*
********************************************************************************************************/


#ifndef _bandpass_h
#define _bandpass_h
#include "firmin.h"
typedef struct _bandpass
{
	int run;
	int position;
	int size;
	int nc;
	int mp;
	double* in;
	double* out;
	double f_low;
	double f_high;
	double samplerate;
	int wintype;
	double gain;
	FIRCORE p;
}bandpass, *BANDPASS;

extern BANDPASS create_bandpass (int run, int position, int size, int nc, int mp, double* in, double* out, 
	double f_low, double f_high, int samplerate, int wintype, double gain);

extern void destroy_bandpass (BANDPASS a);

extern void flush_bandpass (BANDPASS a);

extern void xbandpass (BANDPASS a, int pos);

extern void setBuffers_bandpass (BANDPASS a, double* in, double* out);

extern void setSamplerate_bandpass (BANDPASS a, int rate);

extern void setSize_bandpass (BANDPASS a, int size);

extern void setGain_bandpass (BANDPASS a, double gain, int update);

extern void CalcBandpassFilter (BANDPASS a, double f_low, double f_high, double gain);

extern __declspec (dllexport) void SetRXABandpassFreqs (int channel, double f_low, double f_high);

extern __declspec (dllexport) void SetRXABandpassNC (int channel, int nc);

extern __declspec (dllexport) void SetRXABandpassMP (int channel, int mp);

extern __declspec (dllexport) void SetTXABandpassNC (int channel, int nc);

extern __declspec (dllexport) void SetTXABandpassMP (int channel, int mp);

#endif
