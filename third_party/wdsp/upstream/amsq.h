/*  amsq.h

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
#ifndef _amsq_h
#define _amsq_h

typedef struct _amsq
{
	int run;							// 0 if squelch system is OFF; 1 if it's ON
	int size;							// size of input/output buffers
	double* in;							// squelch input signal buffer
	double* out;						// squelch output signal buffer
	double* trigger;					// pointer to trigger data source
	double* trigsig;					// buffer containing trigger signal
	double rate;						// sample rate
	double avtau;						// time constant for averaging noise
	double avm;						
	double onem_avm;
	double avsig;
	int state;							// state machine control
	int count;
	double tup;
	double tdown;
	int ntup;
	int ntdown;
	double* cup;
	double* cdown;
	double tail_thresh;
	double unmute_thresh;
	double min_tail;
	double max_tail;
	double muted_gain;
} amsq, *AMSQ;

extern AMSQ create_amsq (int run, int size, double* in, double* out, double* trigger, int rate, double avtau, double tup, double tdown, double tail_thresh, double unmute_thresh, double min_tail, double max_tail, double muted_gain);

extern void destroy_amsq (AMSQ a);

extern void flush_amsq (AMSQ a);

extern void xamsq (AMSQ a);

extern void xamsqcap (AMSQ a);

extern void setBuffers_amsq (AMSQ a, double* in, double* out, double* trigger);

extern void setSamplerate_amsq (AMSQ a, int rate);

extern void setSize_amsq (AMSQ a, int size);

// RXA Properties

extern __declspec (dllexport) void SetRXAAMSQRun (int channel, int run);

extern __declspec (dllexport) void SetRXAAMSQThreshold (int channel, double threshold);

extern __declspec (dllexport) void SetRXAAMSQMaxTail (int channel, double tail);

// TXA Properties

extern __declspec (dllexport) void SetTXAAMSQRun (int channel, int run);

extern __declspec (dllexport) void SetTXAAMSQMutedGain (int channel, double dBlevel);

extern __declspec (dllexport) void SetTXAAMSQThreshold (int channel, double threshold);

#endif