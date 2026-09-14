/*  nnr.h

Neural Noise Reduction (NNR) - streaming framework

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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

#ifndef _nnr_h
#define _nnr_h

typedef struct _nnr* NNR;

extern NNR create_nnr
	(
	int run,						// 0 => bypass, 1 => enable
	int position,					// position in the processing chain
	int size,						// input/output buffer size, complex samples
	double* in_buff,				// pointer to input buffer
	double* out_buff,				// pointer to output buffer
	int rate,						// dsp sample rate, Hz
	int nrate,						// network sample rate, Hz (e.g. 16000)
	int fftsize,					// STFT size at nrate (e.g. 512)
	int overlap,					// 2 => 50% overlap, 4 => 75% overlap
	int lookahead,					// network lookahead, in frames (0, 1, 2 ...)
	double mask_floor,				// minimum mask magnitude, dB (e.g. -20.0)
	int cmode						// I/Q handling; see notes in nnr.c
	);

extern void destroy_nnr (NNR a);

extern int setModel_nnr (NNR a, int slot);

extern int getModel_nnr (NNR a);

extern void flush_nnr (NNR a);

extern void xnnr (NNR a, int pos);

extern void setBuffers_nnr (NNR a, double* in, double* out);

extern void setSamplerate_nnr (NNR a, int rate);

extern void setSize_nnr (NNR a, int size);

// diagnostics - returns algorithmic delay through the block, in dsp_rate samples
extern int getDelay_nnr (NNR a);

extern int getRun_nnr(NNR a);


// RXA Properties

extern __declspec (dllexport) void SetRXANNRRun (int channel, int setit);

extern __declspec (dllexport) void SetRXANNRPosition (int channel, int position);

extern __declspec (dllexport) void SetRXANNRMaskFloor (int channel, double floor_db);

extern __declspec (dllexport) void SetRXANNRcmode (int channel, int cmode);

extern __declspec (dllexport) void SetRXANNRTestMode (int channel, int mode);

extern __declspec (dllexport) void SetRXANNRAlpha (int channel, double alpha);

extern __declspec (dllexport) void SetRXANNRAlphaKnee (int channel, double knee_db);

extern __declspec (dllexport) void SetRXANNRTau (int channel, double tau);

extern __declspec (dllexport) void SetRXANNRMaxGain (int channel, double gmax_db);

extern __declspec (dllexport) void SetRXANNRSmooth (int channel, double att_ms, double rel_ms);

extern __declspec (dllexport) int SetRXANNRModel (int channel, int slot);

extern __declspec (dllexport) int GetRXANNRModel (int channel);

#endif
