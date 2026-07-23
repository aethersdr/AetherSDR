/*  osctrl.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2014 Warren Pratt, NR0V

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

// This file is part of the implementation of the Overshoot Controller from
// "Controlled Envelope Single Sideband" by David L. Hershberger, W9GR, in
// the November/December 2014 issue of QEX.

#ifndef _osctrl_h
#define _osctrl_h

typedef struct _osctrl
{
	int run;						// 1 to run; 0 otherwise
	int size;						// buffer size
	double *inbuff;					// input buffer
	double *outbuff;				// output buffer
	int rate;						// sample rate
	double osgain;					// gain applied to overshoot "clippings"
	double bw;						// bandwidth
	int pn;							// "peak stretcher" window, samples
	int dl_len;						// delay line length, samples
	double* dl;						// delay line for complex samples
	double* dlenv;					// delay line for envelope values
	int in_idx;						// input index for dl
	int out_idx;					// output index for dl
	double max_env;					// maximum env value in env delay line
	double env_out;
} osctrl, *OSCTRL;

extern void xosctrl (OSCTRL a);

extern OSCTRL create_osctrl (
				int run,
				int size,
				double* inbuff,
				double* outbuff,
				int rate,
				double osgain );

extern void destroy_osctrl (OSCTRL a);

extern void flush_osctrl (OSCTRL a);

extern void setBuffers_osctrl (OSCTRL a, double* in, double* out);

extern void setSamplerate_osctrl (OSCTRL a, int rate);

extern void setSize_osctrl (OSCTRL a, int size);

#endif