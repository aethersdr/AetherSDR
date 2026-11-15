/*  cmath.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2025 Warren Pratt, NR0V

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

#ifndef _cmath_h
#define _cmath_h

extern double mag (double* value);

extern void cadd (double* a, double* b, double* sum);

extern void csub (double* a, double* b, double* diff);

extern void cmult (double* a, double* b, double* product);

extern void cdiv (double* a, double* b, double* quotient);

extern void cpar (double* Z1, double* Z2, double* Zpar);

extern void cser_to_par (double* Z1, double* ZR, double* ZX);

#endif