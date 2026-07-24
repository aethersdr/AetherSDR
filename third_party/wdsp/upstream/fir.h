/*  fir.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2016, 2022, 2025, 2026 Warren Pratt, NR0V

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

// forward declarations
typedef struct _minphase* MINPHASE;
typedef struct _fsamp* FSAMP;

extern double* fftcv_mults (int NM, double* c_impulse);

extern double* fir_fsamp_odd (int N, double* A, int rtype, double scale, int wintype);

extern double* fir_fsamp (int N, double* A, int rtype, double scale, int wintype);

extern double* fir_bandpass (int N, double f_low, double f_high, double samplerate, int wintype, int rtype, double scale);

extern double *fir_read (int N, const char *filename, int rtype, double scale);

extern void analytic (int N, double* in, double* out);

extern void mp_imp (int N, double* fir, double* mpfir, int pfactor, int polarity);

extern double* zff_impulse (int nc, double scale);

extern MINPHASE create_minphase (int N, int pfactor);

extern void destroy_minphase (MINPHASE a);

extern void mp_imp_exec (MINPHASE a, double* fir, double* mpfir);

extern FSAMP create_fsamp (int N, int wintype);

extern void destroy_fsamp (FSAMP a);

extern void fsamp_exec (FSAMP a, double* A, double* coef, int rtype, double scale);
