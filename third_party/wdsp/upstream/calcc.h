/*  calcc.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2023, 2026 Warren Pratt, NR0V

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

#ifndef _calcc_h
#define _calcc_h

typedef struct _calcc* CALCC;

extern CALCC create_calcc(int channel, int runcal, int size, int rate, double hw_scale,
	double moxdelay, double loopdelay, int mox);

extern void destroy_calcc (CALCC a);

extern void flush_calcc (CALCC a);

extern __declspec(dllexport) void pscc (int channel, int size, double* tx, double* rx);

#endif

// 'info' assignments:
//		 0 - builder for rx_scale
//				0b0001 = Extrapolate_Res.confidence  ('0' good; '1' reverted to linear)
//		 1 - builder for cm
//				0b0001 = nf_fit() Failed.		 [NURB Curve]
//				0b0010 = nf_fit() Quality bad.   [NURB Curve] 
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = count_extrema() Failed. 
//          0b00100000 = MAG Cold Start Error
//		 2 - builder for cc
//              0b0001 = nf_fit() Failed.		 [NURB Curve]
// 			    0b0010 = nf_fit() Quality bad.   [NURB Curve]
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = sin^2 + cos^2 Error.
//          0b00100000 = COS Cold Start Error
//		 3 - builder for cs
//              0b0001 = nf_fit() Failed.		 [NURB Curve]
// 			    0b0010 = nf_fit() Quality bad.   [NURB Curve]
//              0b0100 = ns_build() Failed.      [Spline]
//              0b1000 = ns_build() Quality bad. [Spline]
//          0b00010000 = sin^2 + cos^2 Error.
//          0b00100000 = SIN Cold Start Error
//		 4 - feedback level
//		 5 - count of successful calibrations
//		 6 - scheck() - Final Solution Checks
//              0b0001 = New-Old Soln Compare Check
// 			    0b0010 = Can't FIll Buckets, Probable Over-Drive
//		 7 - count of attempted calibrations
// 
//      12 - file write/read error
//		13 - 
//		14 - indicates iqc_Run = 1
//		15 - control state
//
//
//      ** => A non-zero value in binfo[0], binfo[1], binfo[2], binfo[3], or binfo[6]
//            sets a->scOK==0 and the solution is rejected.
