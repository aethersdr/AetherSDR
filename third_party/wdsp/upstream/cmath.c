/*  cmath.c

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

#include "comm.h"

// function to calculate the magnitude of a complex value.
double mag(double* value)
{
	return sqrt(value[0] * value[0] + value[1] * value[1]);
}

// function to perform a Complex Add, a+b; it returns a complex value, 'sum'
void cadd(double* a, double* b, double* sum)
{
	sum[0] = a[0] + b[0];
	sum[1] = a[1] + b[1];
}

// function to perform a Complex Subtract, a-b; it returns a complex value, 'diff'
void csub(double* a, double* b, double* diff)
{
	diff[0] = a[0] - b[0];
	diff[1] = a[1] - b[1];
}

// function to perform a Complex Multiply, a*b; it returns a complex value, 'product'
void cmult(double* a, double* b, double* product)
{
	product[0] = a[0] * b[0] - a[1] * b[1];
	product[1] = a[0] * b[1] + a[1] * b[0];
}

// function to perform a Complex Divide, a/b; it returns a complex value, 'quotient'
void cdiv(double* a, double* b, double* quotient)
{
	double den = b[0] * b[0] + b[1] * b[1];
	quotient[0] = (a[0] * b[0] + a[1] * b[1]) / den;
	quotient[1] = (a[1] * b[0] - a[0] * b[1]) / den;
}

// function to calculate complex Z (series equivalent) of two parallel elements
void cpar(double* Z1, double* Z2, double* Zpar)
{
	double num[2], den[2];
	cmult(Z1, Z2, num);
	cadd(Z1, Z2, den);
	cdiv(num, den, Zpar);
}

// function to convert a complex Z to parallel R and X values
void cser_to_par(double* Z1, double* ZR, double* ZX)
	{
	// Z1 is the sum of real and imaginary (resistive and reactive) components
	// While expressed as complex, ZR contains the resistive parallel element with imaginary component equal to zero
	// While expressed as complex, ZX contains the reactive parallel element with the real component equal to zero
	double num = Z1[0] * Z1[0] + Z1[1] * Z1[1];
	ZR[0] = num / Z1[0];
	ZR[1] = 0.0;
	ZX[0] = 0.0;
	ZX[1] = num / Z1[1];
	}