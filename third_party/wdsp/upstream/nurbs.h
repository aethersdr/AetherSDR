/*  nurbs.h

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

#ifndef _nurbs_h
#define _nurbs_h

typedef struct _nurbs
{
	int n;
	int p;
	int r;
	int umethod;
	double* U;
	double* CP;
	double* W;
	int upts;
	double* Xs;
	double* Ys;
	double* Uout;
	int fpts;
	double* Xf; 
	double* Yf;
	int max_cp;
	int max_p;
	int max_upts;
	int max_fpts;
	int max_knots;
} nurbs, *NURBS;

extern NURBS create_nurbs(int n, int p, int r, int umethod, int upts,
	int max_cp, int max_deg, int max_upts, int max_fpts);

extern void destroy_nurbs (NURBS a);

extern void BuildSpline (int n, int p, int r, int umethod, double* U, double* CP, double* W,
	int upts, double* Xs, double* Ys, double* Uout, int fpts, double* Xf, double* Yf);

extern int checkSplineInputs (int ncp, int p, int r, int umethod, double* W);

#endif
