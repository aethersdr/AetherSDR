/*  matchedCW.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2025 Warren Pratt, NR0V

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

/********************************************************************************************************
*																										*
*									Partitioned Overlap-Save Gaussian									*
*																										*
********************************************************************************************************/


#ifndef _matchedCW_h
#define _matchedCW_h
#include "firmin.h"
typedef struct _matchedCW
{
	int run;					// 0 - filter is OFF; 1 - filter is ON
	int position;				// position in sequence in which to execute the filter
	int size;					// input/output buffer size
	int nc;						// number of impulse response coefficients
	double* in;					// pointer to input buffer
	double* out;				// pointer to output buffer
	double f_center;			// filter center frequency (Hz)
	double bandwidth;			// filter bandwidth (Hz)
	double samplerate;			// sample_rate (samples/sec)
	double gain;				// gain to be applied to filter output
	double scale;				// internal filter scale factor based upon gain
	int mode;					// Mode to get output:  0 => CWL; 1 => CWU; 2 => CWL + CWU
	FIRCORE p;
} matched, *MATCHED;

extern MATCHED create_matched (int run, int position, int size, double* in, double* out,
	double f_center, double bandwidth, int samplerate, double gain, int mode);

extern void destroy_matched (MATCHED a);

extern void flush_matched (MATCHED a);

extern void xmatched (MATCHED a, int pos);

extern void setBuffers_matched (MATCHED a, double* in, double* out);

extern void setSamplerate_matched (MATCHED a, int rate);

extern void setSize_matched (MATCHED a, int size);

extern void setGain_matched (MATCHED a, double gain);

extern void CalcMatchedFilter(MATCHED a, double f_center, double bandwidth, double gain);

extern __declspec (dllexport) void SetRXAMatchedRun (int channel, int run);

extern __declspec (dllexport) void SetRXAMatchedFreqs (int channel, double f_center, double bandwidth);

extern __declspec (dllexport) void SetRXAMatchedGain (int channel, double gain);

#endif
