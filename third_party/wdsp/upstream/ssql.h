/*  ssql.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2023 Warren Pratt, NR0V

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

#ifndef _ssql_h
#define _ssql_h

typedef struct _ftov					// Frequency to Voltage Converter
{
	int run;							// 0 => don't run; 1 => run
	int size;							// buffer size
	int rate;							// sample-rate
	int rsize;							// rate * time_to_fill_ring, e.g., 48K/s * 50ms = 2400
	double fmax;						// frequency (Hz) for full output, e.g., 2000 (Hz)
	double* in;							// pointer to the intput buffer for ftov
	double* out;						// pointer to the output buffer for ftov
	int* ring;							// pointer to the base of the ring
	int rptr;							// index into the ring
	double inlast;						// holds last sample from previous buffer
	int rcount;							// count of zero-crossings currently in the ring
	double div;							// divisor for 'rcount' to produce output of 1.0 at 'fmax'
	double eps;							// minimum input change to count as a signal edge transition
} ftov, *FTOV;

typedef struct _ssql					// Syllabic Squelch
{
	int run;							// 0 if squelch system is OFF; 1 if it's ON
	int size;							// size of input/output buffers
	double* in;							// squelch input signal buffer
	double* out;						// squelch output signal buffer
	int rate;							// sample rate
	int state;							// state machine control
	int count;							// count variable for raised cosine transitions
	double tup;							// time for turn-on transition
	double tdown;						// time for turn-off transition
	int ntup;							// number of samples for turn-on transition
	int ntdown;							// number of samples for turn-off transition
	double* cup;						// coefficients for up-slew
	double* cdown;						// coefficients for down-slew
	double muted_gain;					// audio gain while muted; 0.0 for complete silence

	double* b1;							// buffer to hold output of dc-block function
	double* ibuff;						// buffer containing only 'I' component
	double* ftovbuff;					// buffer containing output of f to v converter
	double* lpbuff;						// buffer containing output of low-pass filter
	int* wdbuff;						// buffer containing output of window detector
	CBL dcbl;							// pointer to DC Blocker data structure
	FTOV cvtr;							// pointer to F to V Converter data structure
	BQLP filt;							// pointer to Bi-Quad Low-Pass Filter data structure
	int ftov_rsize;						// ring size for f_to_v converter
	double ftov_fmax;					// fmax for f_to_v converter
	// window detector
	double wdtau;						// window detector time constant
	double wdmult;						// window detector time constant multiplier
	double wdaverage;					// average signal value
	double wthresh;						// window threshold above/below average
	// trigger
	double tr_thresh;					// trigger threshold:  100K/(100K+22K)=0.8197
	double tr_tau_unmute;				// trigger unmute time-constant:  (100K||220K)*10uF = 0.6875
	double tr_ss_unmute;				// trigger steady-state level for unmute:  100K/(100K+220K)=0.3125
	double tr_tau_mute;					// trigger mute time-constant:  220K*10uF = 2.2
	double tr_ss_mute;					// trigger steady-state level for mute:  1.0
	double tr_voltage;					// trigger voltage
	double mute_mult;					// multiplier for successive voltage calcs when muted
	double unmute_mult;					// multiplier for successive voltage calcs when unmuted
	int* tr_signal;						// trigger signal, 0 or 1
} ssql, * SSQL;

extern SSQL create_ssql (int run, int size, double* in, double* out, int rate, double tup, double tdown,
	double muted_gain, double tau_mute, double tau_unmute, double wthresh, double tr_thresh, int rsize, double fmax);

extern void destroy_ssql (SSQL a);

extern void flush_ssql (SSQL a);

extern void xssql (SSQL a);

extern void setBuffers_ssql (SSQL a, double* in, double* out);

extern void setSamplerate_ssql (SSQL a, int rate);

extern void setSize_ssql (SSQL a, int size);

#endif
