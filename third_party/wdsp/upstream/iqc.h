/*  iqc.h

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

#ifndef _iqc_h
#define _iqc_h
#include "nurbs_spline.h"
typedef struct _iqc
{
	NS_Spline   *m_spline[2], *c_spline[2], *s_spline[2];
	CurveEMA    m_calavg[2], c_calavg[2], s_calavg[2];
	double      m_prev_y[2], c_prev_y[2], s_prev_y[2];

	volatile long run;
	volatile long busy;
	int size;
	double* in;
	double* out;
	double rate;
	int cset;
	double tup;
	double* cup;
	int count;
	int ntup;
	int state;
	
} iqc, *IQC;

extern IQC create_iqc(int run, int size, double* in, double* out, double rate, double tup);

extern void destroy_iqc (IQC a);

extern void flush_iqc (IQC a);

extern void xiqc (IQC a);

extern void setBuffers_iqc (IQC a, double* in, double* out);

extern void setSamplerate_iqc (IQC a, int rate);

extern void setSize_iqc (IQC a, int size);

// TXA Properties

extern void GetTXAiqcValues(int channel, 
	NS_Spline** m_spline, CurveEMA* m_calavg, double* m_prev_y,
	NS_Spline** c_spline, CurveEMA* c_calavg, double* c_prev_y,
	NS_Spline** s_spline, CurveEMA* s_calavg, double* s_prev_y);

extern void SetTXAiqcSwap(int channel, 
	NS_Spline* m_spline, CurveEMA* m_calavg, double m_prev_y,
	NS_Spline* c_spline, CurveEMA* c_calavg, double c_prev_y,
	NS_Spline* s_spline, CurveEMA* s_calavg, double s_prev_y);

extern void SetTXAiqcStart(int channel, 
	NS_Spline* m_spline, CurveEMA* m_calavg, double m_prev_y,
	NS_Spline* c_spline, CurveEMA* c_calavg, double c_prev_y,
	NS_Spline* s_spline, CurveEMA* s_calavg, double s_prev_y);

extern void SetTXAiqcEnd (int channel);

#endif
