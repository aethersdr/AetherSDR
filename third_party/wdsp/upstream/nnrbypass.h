/*  nnrbypass.h

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

#ifndef _nnrbypass_h
#define _nnrbypass_h

typedef struct _nnrbypass* NNRBYPASS;

extern NNRBYPASS create_nnrbypass(int run, int size, int nc_default, double* in,
	double* out, double f_low, double f_high, int samplerate, double gain);

extern void destroy_nnrbypass (NNRBYPASS b);

extern void flush_nnrbypass (NNRBYPASS b);

extern void xnnrbypin (NNRBYPASS b);

extern void xnnrbypout (NNRBYPASS b);

extern void setBuffers_nnrbypass (NNRBYPASS b, double* in, double* out);

extern void setSamplerate_nnrbypass (NNRBYPASS b, int rate);

extern void setSize_nnrbypass (NNRBYPASS b, int size);

extern void setRun_nnrbypass (NNRBYPASS b, int run, double f_low, double f_high);

extern int getRun_nnrbypass (NNRBYPASS b);

extern double getBandwidth_nnrbypass (NNRBYPASS b);

#endif

