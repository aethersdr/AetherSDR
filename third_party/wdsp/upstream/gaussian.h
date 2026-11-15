/*  gaussian.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2025-2026 Warren Pratt, NR0V

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


#ifndef _gaussian_h
#define _gaussian_h
#include "firmin.h"
typedef struct _gaussian
{
	int run;					// 0 - filter is OFF; 1 - filter is ON
	int position;				// position in sequence in which to execute the filter
	int size;					// input/output buffer size
	int nc;						// number of filter coefficients; if set to '0', will be automatically calculated
	int nc_default;				// value to which 'nc' was last set at instantiation or by a call to set 'nc'
	double* in;					// pointer to input buffer
	double* out;				// pointer to output buffer
	double f_center;			// filter center frequency (Hz)
	double bandwidth;			// filter bandwidth (Hz)
	double samplerate;			// sample_rate (samples/sec)
	double gain;				// gain to be applied to filter output
	double scale;				// internal filter scale factor based upon gain
	double nsigma;				// number of 'sigma' on each side of center to extend the Gaussian curve
	int mode;					// Mode to get output:  0 => CWL; 1 => CWU; 2 => CWL + CWU
	FIRCORE p;					// pointer to partititioned overlap-save filter
}gaussian, *GAUSSIAN;

extern GAUSSIAN create_gaussian(int run, int position, int size, int nc, double* in, double* out,
	double f_center, double bandwidth, int samplerate, double gain, double nsigma, int mode);

extern void destroy_gaussian(GAUSSIAN a);

extern void flush_gaussian(GAUSSIAN a);

extern void xgaussian(GAUSSIAN a, int pos);

extern void setBuffers_gaussian(GAUSSIAN a, double* in, double* out);

extern void setSamplerate_gaussian(GAUSSIAN a, int rate);

extern void setSize_gaussian(GAUSSIAN a, int size);

extern void setGain_gaussian(GAUSSIAN a, double gain);

extern void CalcGaussianFilter(GAUSSIAN a, double f_center, double bandwidth, double gain);

extern __declspec (dllexport) void SetRXAGaussianRun(int channel, int run);

extern __declspec (dllexport) void SetRXAGaussianFreqs(int channel, double f_center, double bandwidth);

extern __declspec (dllexport) void SetRXAGaussianGain(int channel, double gain);

extern __declspec (dllexport) void SetRXAGaussianNC(int channel, int nc);

#endif
