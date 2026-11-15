/*  iir.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014, 2022, 2023, 2026 Warren Pratt, NR0V

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
*											Bi-Quad Notch												*
*																										*
********************************************************************************************************/

#ifndef _snotch_h
#define _snotch_h

typedef struct _snotch
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double f;
	double bw;
	double a0, a1, a2, b1, b2;
	double x0, x1, x2, y1, y2;
	CRITICAL_SECTION cs_update;
} snotch, *SNOTCH;

extern SNOTCH create_snotch (int run, int size, double* in, double* out, int rate, double f, double bw);

extern void destroy_snotch (SNOTCH a);

extern void flush_snotch (SNOTCH a);

extern void xsnotch (SNOTCH a);

extern void setBuffers_snotch (SNOTCH a, double* in, double* out);

extern void setSamplerate_snotch (SNOTCH a, int rate);

extern void setSize_snotch (SNOTCH a, int size);

extern void SetSNCTCSSFreq (SNOTCH a, double freq);

extern void SetSNCTCSSRun (SNOTCH a, int run);

#endif

/********************************************************************************************************
*																										*
*											Complex Bi-Quad Peaking										*
*																										*
********************************************************************************************************/

#ifndef _speak_h
#define _speak_h

typedef struct _speak
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double f;
	double bw;
	double cbw;
	double gain;
	double fgain;
	int nstages;
	int design;
	double a0, a1, a2, b1, b2;
	double *x0, *x1, *x2, *y0, *y1, *y2;
	CRITICAL_SECTION cs_update;
} speak, *SPEAK;

extern SPEAK create_speak (int run, int size, double* in, double* out, int rate, double f, double bw, double gain, int nstages, int design);

extern void destroy_speak (SPEAK a);

extern void flush_speak (SPEAK a);

extern void xspeak (SPEAK a);

extern void setBuffers_speak (SPEAK a, double* in, double* out);

extern void setSamplerate_speak (SPEAK a, int rate);

extern void setSize_speak (SPEAK a, int size);

extern __declspec (dllexport) void SetRXABiQuadRun(int channel, int run);

extern __declspec (dllexport) void SetRXABiQuadFreq(int channel, double freq);

extern __declspec (dllexport) void SetRXABiQuadBandwidth(int channel, double bw);

extern __declspec (dllexport) void SetRXABiQuadGain(int channel, double gain);

#endif

/********************************************************************************************************
*																										*
*										Complex Multiple Peaking										*
*																										*
********************************************************************************************************/

#ifndef _mpeak_h
#define _mpeak_h

typedef struct _mpeak
{
	int run;
	int size;
	double* in;
	double* out;
	int rate;
	int npeaks;
	int* enable;
	double* f;
	double* bw;
	double* gain;
	int nstages;
	SPEAK* pfil;
	double* tmp;
	double* mix;
	CRITICAL_SECTION cs_update;
} mpeak, *MPEAK;

extern MPEAK create_mpeak (int run, int size, double* in, double* out, int rate, int npeaks, int* enable, double* f, double* bw, double* gain, int nstages);

extern void destroy_mpeak (MPEAK a);

extern void flush_mpeak (MPEAK a);

extern void xmpeak (MPEAK a);

extern void setBuffers_mpeak (MPEAK a, double* in, double* out);

extern void setSamplerate_mpeak (MPEAK a, int rate);

extern void setSize_mpeak (MPEAK a, int size);

#endif

/********************************************************************************************************
*																										*
*									Complex Bi-Quad Low-Pass				     						*
*																										*
********************************************************************************************************/

#ifndef _bqlp_h
#define _bqlp_h

typedef struct _bqlp
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double fc;
	double Q;
	double gain;
	int nstages;
	double a0, a1, a2, b1, b2;
	double* x0, * x1, * x2, * y0, * y1, * y2;
	CRITICAL_SECTION cs_update;
} bqlp, *BQLP;

extern BQLP create_bqlp(int run, int size, double* in, double* out, double rate, double fc, double Q, double gain, int nstages);

extern void destroy_bqlp(BQLP a);

extern void flush_bqlp(BQLP a);

extern void xbqlp(BQLP a);

extern void setBuffers_bqlp(BQLP a, double* in, double* out);

extern void setSamplerate_bqlp(BQLP a, int rate);

extern void setSize_bqlp(BQLP a, int size);

#endif

/********************************************************************************************************
*																										*
*									   Double Bi-Quad Low-Pass				     						*
*																										*
********************************************************************************************************/

#ifndef _dbqlp_h
#define _dbqlp_h

extern BQLP create_dbqlp(int run, int size, double* in, double* out, double rate, double fc, double Q, double gain, int nstages);

extern void destroy_dbqlp(BQLP a);

extern void flush_dbqlp(BQLP a);

extern void xdbqlp(BQLP a);

extern void setBuffers_dbqlp(BQLP a, double* in, double* out);

extern void setSamplerate_dbqlp(BQLP a, int rate);

extern void setSize_dbqlp(BQLP a, int size);

#endif

/********************************************************************************************************
*																										*
*									Complex Bi-Quad Band-Pass				     						*
*																										*
********************************************************************************************************/

#ifndef _bqbp_h
#define _bqbp_h

typedef struct _bqbp
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double f_low;
	double f_high;
	double gain;
	int nstages;
	double a0, a1, a2, b1, b2;
	double* x0, * x1, * x2, * y0, * y1, * y2;
	CRITICAL_SECTION cs_update;
} bqbp, * BQBP;

extern BQBP create_bqbp(int run, int size, double* in, double* out, double rate, double f_low, double f_high, double gain, int nstages);

extern void destroy_bqbp(BQBP a);

extern void flush_bqbp(BQBP a);

extern void xbqbp(BQBP a);

extern void setBuffers_bqbp(BQBP a, double* in, double* out);

extern void setSamplerate_bqbp(BQBP a, int rate);

extern void setSize_bqbp(BQBP a, int size);

#endif

/********************************************************************************************************
*																										*
*									  Double Bi-Quad Band-Pass				     						*
*																										*
********************************************************************************************************/

#ifndef _dbqbp_h
#define _dbqbp_h

extern BQBP create_dbqbp(int run, int size, double* in, double* out, double rate, double f_low, double f_high, double gain, int nstages);

extern void destroy_dbqbp(BQBP a);

extern void flush_dbqbp(BQBP a);

extern void xdbqbp(BQBP a);

extern void setBuffers_dbqbp(BQBP a, double* in, double* out);

extern void setSamplerate_dbqbp(BQBP a, int rate);

extern void setSize_dbqbp(BQBP a, int size);

#endif

/********************************************************************************************************
*																										*
*									   Double Single-Pole High-Pass				   						*
*																										*
********************************************************************************************************/

#ifndef _dsphp_h
#define _dsphp_h

typedef struct _sphp
{
	int run;
	int size;
	double* in;
	double* out;
	double rate;
	double fc;
	int nstages;
	double a1, b0, b1;
	double* x0, * x1, * y0, * y1;
	CRITICAL_SECTION cs_update;
} sphp, * SPHP;

extern SPHP create_dsphp(int run, int size, double* in, double* out, double rate, double fc, int nstages);

extern void destroy_dsphp(SPHP a);

extern void flush_dsphp(SPHP a);

extern void xdsphp(SPHP a);

extern void setBuffers_dsphp(SPHP a, double* in, double* out);

extern void setSamplerate_dsphp(SPHP a, int rate);

extern void setSize_dsphp(SPHP a, int size);

#endif

/********************************************************************************************************
*																										*
*								     Complex Single-Pole High-Pass				     					*
*																										*
********************************************************************************************************/

#ifndef _dphp_h
#define _dphp_h

extern SPHP create_sphp(int run, int size, double* in, double* out, double rate, double fc, int nstages);

extern void destroy_sphp(SPHP a);

extern void flush_sphp(SPHP a);

extern void xsphp(SPHP a);

extern void setBuffers_sphp(SPHP a, double* in, double* out);

extern void setSamplerate_sphp(SPHP a, int rate);

extern void setSize_sphp(SPHP a, int size);

#endif