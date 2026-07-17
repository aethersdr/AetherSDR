/*  extrapolate.h

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

#ifndef _extrapolate_h
#define _extrapolate_h

typedef enum {
	EXTRAP_CONFIDENT = 0,   
	EXTRAP_LOW_CONFIDENCE = 1   
} ExtrapolationConfidence;

typedef struct {
	double                 y_at_1;        
	ExtrapolationConfidence confidence;   
	double                 y_linear;      
	double                 y_quadratic;   
	double                 y_cubic;       
} ExtrapolationResult;

extern ExtrapolationResult extrapolate_y_at_1(const double* x, const double* y, int n);

extern ExtrapolationResult extrapolate_y_at_0(const double* x, const double* y,
                                               int n,
                                               double x_lo,
                                               double x_head_max);

#endif

