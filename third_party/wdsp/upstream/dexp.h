/*  dexp.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2018, 2019 Warren Pratt, NR0V

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

#ifndef _dexp_h
#define _dexp_h

typedef struct _delring
{
	int rsize;							// ringsize (measured in complex samples)
	double* ring;						// ring buffer
	int inptr;							// ring input pointer (counts in complex samples)
	int outptr;							// ring output pointer (counts in complex samples)
	int rdelay;							// ring delay (measured in complex samples)
	int size;							// input/output size in complex samples
	double* in;							// source buffer
	double* out;						// destination buffer
} delring, *DELRING;

typedef struct _dexp
{
	int id;								// 'id' for this dexp
	int run_dexp;						// 0 if dexp is OFF; 1 if it's ON
	int size;							// size of input/output buffers
	double* in;							// audio input buffer
	double* out;						// audio output buffer; can be same as 'in'
	double rate;						// sample rate
	double dettau;						// detection averaging time constant
	double avm;							// averaging multiplier
	double onem_avm;					// one minus averaging multiplier
	double avsig;						// averaged detection signal
	int state;							// state machine control
	int count;							// count variable used within a state
	double tattack;						// attack time
	double tdecay;						// decay time
	int nattack;						// one less than total number of attack multipliers
	int ndecay;							// one less than total number of decay multipliers
	double* cattack;					// attack curve multipliers
	double* cdecay;						// decay curve multipliers
	double attack_thresh;				// attack threshold
	double hold_thresh;					// hold & decay threshold
	double thold;						// hold time
	int nhold;							// hold count
	double exp_ratio;					// expander ratio (high-gain to low-gain)
	double hysteresis_ratio;			// ratio hold_thresh/attack_thresh.  0.0 < ratio < 1.0
	double low_gain;					// gain when gate is closed
	double* trigsig;					// buffer for trigger signal (signal after side-channel filter)
	double* delsig;						// buffer for signal delayed to match trigger signal
	double peak;						// peak signal value to return to console
	// side-channel bandpass filter & and buffer for compensating delay
	int run_filt;						// 1 = side-channel filter and compensating delay are ON, 0 = OFF
	int nc;								// number of coefficients
	int wintype;						// window type
	double low_cut;						// low cutoff frequency
	double high_cut;					// high cutoff frequency
	FIRCORE p;							// filter structure
	DELRING scdring;					// delay ring for side channel
	// output audio delay to cover RF_Delay + Xmtr_delay_and_upslew
	double* audbuffer;					// buffer to serve as input to audring
	int run_audelay;					// 'run' variable for audio delay ring
	double audelay;						// audio output delay in seconds
	DELRING audring;					// audio delay ring
	// vox
	int run_vox;
	void (__stdcall *pushvox)(int channel, int active);
	int vox_count;
	// update critical section
	CRITICAL_SECTION cs_update;
	// anti-vox
	int antivox_run;					// 'run' for anti-vox
	int antivox_new;					// internal variable indicating new anti-vox data is available
	int antivox_size;					// size of anti-vox data buffer
	double antivox_rate;				// sample-rate of anti-vox data
	double antivox_tau;					// time-constant of anti-vox smoothing
	double antivox_gain;				// anti-vox gain factor
	double antivox_mult;				// multiplier for anti-vox smoothing
	double antivox_onemmult;			// one minus antivox_mult
	double antivox_level;				// current anti-vox smoothed signal level
	double* antivox_data;				// buffer to hold new anti-vox data
} dexp, *DEXP;

extern DEXP pdexp[];

__declspec (dllexport) void create_dexp (int id, int run_dexp, int size, double* in, double* out, int rate, double dettau, double tattack, double tdecay, 
	double thold, double exp_ratio, double hyst_ratio, double attack_thresh, int nc, int wtype, double lowcut, double highcut, 
	int run_filt, int run_vox, int run_audelay, double audelay, void (__stdcall *pushvox)(int id, int active),
	int antivox_run, int antivox_size, int antivox_rate, double antivox_gain, double antivox_tau);

__declspec (dllexport) void destroy_dexp (int id);

__declspec (dllexport) void flush_dexp (int id);

__declspec (dllexport) void xdexp (int id);

__declspec (dllexport) void SetDEXPSize (int id, int size);

__declspec (dllexport) void SetDEXPRate (int id, double rate);

__declspec (dllexport) void SendAntiVOXData (int id, int nsamples, double* data);

#endif