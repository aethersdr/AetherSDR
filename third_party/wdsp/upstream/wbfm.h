/*  wbfm.h

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

warren@pratt.one
*/

#ifndef _wbfm_h
#define _wbfm_h

#include "firmin.h"
#include "wcpAGC.h"

//Forward declarations:
//   Full definitions expected elsewhere; this allows
//   wbfm.h to refer to these as pointer types.
typedef struct _wbsql  *WBSQL;
typedef struct _indy   *INDY;
typedef struct _dmph   *DMPH;
typedef struct _sdelay *SDELAY;


typedef struct _wbfm
{
	// the basics
	int run;
	int size;
	double* in;
	double* out;
	double rate;

	// internal buffers
	double* disc_out;
	double* fil19_out;
	double* fil53_out;
	double* LmR;
	double* LpR;
	double* L;
	double* R;

	// discriminator & dcb
	double discSave_I;
	double discSave_Q;
	double dcbSave_x;
	double dcbSave_y;
	double disc_gain_comp;
	
	// filter:  0-15 kHz
	FIRCORE pfil0_15;
	double flow_fil0_15;
	double fhigh_fil0_15;
	int nc_fil0_15;

	// filter:  19 kHz
	FIRCORE pfil19;
	double flow_fil19;
	double fhigh_fil19;
	double magComp_fil19;
	int nc_fil19;

	// filter:  23-53 kHz
	FIRCORE pfil23_53;
	double flow_fil23_53;
	double fhigh_fil23_53;
	int nc_fil23_53;

	// Pilot AGC
	double tau_attack;
	double n_tau;
	double targ_mult;
	double out_targ;
	int agc_delay;
	WCPAGC pAGC_Pilot;

	// Deemphasis Filters
	DMPH dmphL;
	DMPH dmphR;

	// Indicator
	INDY pIndy;

	// Squelch
	WBSQL psql;

	// Miscellaneous
	int stereo;
	double sqgain;
	double mag19;
	int dmph;
	int dmph_type;

	// Matching Delays
	SDELAY del15;
	SDELAY del53;

} wbfm, *WBFM;

extern WBFM create_wbfm(int run, int size, double* in, double* out, double rate);
extern void destroy_wbfm(WBFM a);
extern void xwbfm(WBFM a);
extern void flush_wbfm(WBFM a);
extern void setBuffers_wbfm(WBFM a, double* in, double* out);
extern void setSamplerate_wbfm(WBFM a, int rate);
extern void setSize_wbfm(WBFM a, int size);

#endif

