/*  nurbs.c

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


#include "comm.h"

static int nurbs_find_span(int n, int p, const double* U, double u)
{
	if (u >= U[n + 1])
		return n;
	int s = p;
	while (s < n && u >= U[s + 1])
		s++;

	return s;
}

static void nurbs_basis_funs(int s, int p, const double* U,
	double u, double* N)
{
	double prev[32] = { 0.0 };
	double curr[32] = { 0.0 };

	prev[0] = 1.0;

	int d, r;
	for (d = 1; d <= p; d++)
	{
		for (r = 0; r <= d; r++) curr[r] = 0.0;
		for (r = 0; r <= d; r++)
		{
			if (r > 0)
			{
				double u_lo = U[s - d + r];
				double u_hi = U[s + r];
				double denom = u_hi - u_lo;
				double alpha = (denom > 1e-300) ? (u - u_lo) / denom : 0.0;
				curr[r] += alpha * prev[r - 1];
			}
			if (r < d)
			{
				double u_lo = U[s - d + r + 1];
				double u_hi = U[s + r + 1];
				double denom = u_hi - u_lo;
				double beta = (denom > 1e-300) ? (u - u_lo) / denom : 0.0;
				curr[r] += (1.0 - beta) * prev[r];
			}
		}
		for (r = 0; r <= d; r++) prev[r] = curr[r];
	}
	for (r = 0; r <= p; r++) N[r] = prev[r];
}

/*
 * NURBSpoint()
 *
 * Evaluates a B-spline or NURBS curve at parameter u and returns the
 * Cartesian coordinates (x, y) of the corresponding curve point.
 *
 * INPUTS
 *   n    — highest control-point index; there are n+1 control points
 *   p    — polynomial degree of each span
 *   r    — 0 = non-rational B-spline;  non-zero = rational NURBS
 *   U    — knot vector, length n+p+2, clamped: U[0..p]=0, U[n+1..n+p+1]=1
 *   CP   — control points, interleaved x,y pairs: [x0,y0, x1,y1, ..., xn,yn]
 *   W    — weights, length n+1 (relevant only when r != 0)
 *   u    — curve parameter in [0, 1]
 *
 * OUTPUTS
 *   *x, *y — Cartesian coordinates of the curve point
 */
void NURBSpoint(int n, int p, int r,
	double* U, double* CP, double* W, double u,
	double* x, double* y)
{
	double N[32] = { 0.0 };
	int    s, i;
	s = nurbs_find_span(n, p, U, u);
	nurbs_basis_funs(s, p, U, u, N);
	*x = 0.0;
	*y = 0.0;
	if (r)
	{
		double W_sum = 0.0;
		for (i = 0; i <= p; i++)
			W_sum += N[i] * W[s - p + i];
		if (W_sum < 1e-300) W_sum = 1e-300;

		for (i = 0; i <= p; i++)
		{
			double Ri = N[i] * W[s - p + i] / W_sum;
			*x += Ri * CP[2 * (s - p + i) + 0];
			*y += Ri * CP[2 * (s - p + i) + 1];
		}
	}
	else
	{
		for (i = 0; i <= p; i++)
		{
			*x += N[i] * CP[2 * (s - p + i) + 0];
			*y += N[i] * CP[2 * (s - p + i) + 1];
		}
	}
}


void Space (int inpoints, int outpoints, double* Xin, double* Yin, double* Xout, double* Yout)
{
	double Xrange = Xin[inpoints - 1] - Xin[0];
	if (Xrange < 1e-300) 
	{
		for (int k = 0; k < outpoints; k++) { Xout[k] = Xin[0]; Yout[k] = Yin[0]; }
		return;
	}
	double out_delta = Xrange / (double)(outpoints - 1);
	double Xout_value = Xin[0] + out_delta;
	double in_frac = 0.0;
	int i = 1, j = 1;
	Xout[0] = Xin[0];
	Yout[0] = Yin[0];
	while (j < outpoints)
	{
		if (i >= inpoints)
		{
			Xout[j] = Xin[inpoints - 1];
			Yout[j] = Yin[inpoints - 1];
			j++;
			continue;
		}
		if (Xout_value <= Xin[i])
		{
			in_frac = (Xout_value - Xin[i - 1]) / (Xin[i] - Xin[i - 1]);
			Xout[j] = Xout_value;
			Yout[j] = Yin[i - 1] + in_frac * (Yin[i] - Yin[i - 1]);
			Xout_value += out_delta;
			if (Xout_value > Xin[inpoints - 1]) Xout_value = Xin[inpoints - 1];
			j++;
		}
		else 
			i++;
	}
}

void Ucalc (int n, int p, int umethod, double* U, double* CP)
{
	int m = n + p + 1;
	int total_knots = m + 1;
	int unique_knots = total_knots - 2 * p;
	int i = 0, j = 0;
	double u = 0.0, udelta = 0.0;
	double x_range = 0.0, frac_urange = 0.0, pos_ptsrange = 0.0, pos_frac = 0.0;
	int pos_floor = 0;
	switch (umethod)
	{
	case 0:		// Based upon control points.
		i = 0;
		x_range = CP[2 * n + 0] - CP[0];
		while (i <= p)
			U[i++] = 0.0;
		for (i = p + 1, j = 1; i < p + unique_knots - 1; i++, j++)
		{
			frac_urange = (double)j / (double)(unique_knots - 1);
			pos_ptsrange = frac_urange * (double)n;
			pos_floor = (int)pos_ptsrange;
			pos_frac = pos_ptsrange - (double)pos_floor;
			U[i] = ((1.0 - pos_frac) * CP[2 * pos_floor] + pos_frac * CP[2 * pos_floor + 2]
				- CP[0]) / x_range;
		}
		while (i < total_knots)
			U[i++] = 1.0;
		break;
	case 1:		// Uniformly distributed.
		i = 0;
		udelta = 1.0 / (double)(unique_knots - 1);
		u = udelta;
		while (i <= p)
			U[i++] = 0.0;
		while (i < p + unique_knots - 1)
		{
			U[i++] = u;
			u += udelta;
		}
		while (i < total_knots)
			U[i++] = 1.0;
		break;
	case 2:
	default:
		// Knots have been externally supplied in 'U'.  Do nothing to them.
		break;
	}
}

void BuildSpline (int n, int p, int r, int umethod, double* U, double* CP, double* W, 
	int upts, double* Xs, double* Ys, double* Uout, int fpts, double* Xf, double* Yf)
{
	// n - (n + 1) = number of control points
	// p - degree of each segment
	// r - '1' for (rational) NURBS, '0' for just B-Spline
	// umethod - method to be used to calculate the 'U' knot vector
	// U - knot vector: complete, including (p + 1) replications at beginning and end
	// CP - control point vector in x, y, x, ... order
	// W - weight vector, one weight per control point
	// upts - number of spline points to generate/return, i.e., number of 'u' values to evaluate
	// Xs - 'x' coordinates of returned spline points at the 'u' values
	// Ys - 'y' coordinates of returned spline points at the 'u' values
	// Uout - 'u' values corresponding to 'Xs' and 'Ys' values
	// fpts - number of points with equal 'x' spacing to calculate/return
	// Xf - 'x' coordinates of returned equally-spaced points
	// Yf - 'y' coordinates of returned equally-spaced points
	int m = n + p + 1;
	double u = 0.0;
	if (n < p) 
	{
		for (int i = 0; i < fpts; i++) { Xf[i] = 0.0; Yf[i] = 0.0; }
		return;
	}
	Ucalc(n, p, umethod, U, CP);
	for (int i = 0; i < upts; i++)
	{
		u = ((double)i / (double)(upts - 1)) * (U[m] - U[0]);
		Uout[i] = u;
		NURBSpoint (n, p, r, U, CP, W, u, &Xs[i], &Ys[i]);
	}
	Space (upts, fpts, Xs, Ys, Xf, Yf);
}

int checkSplineInputs(int ncp, int p, int r, int umethod, double* W)
{
	int warning = 0b0000;
	// IF using splines
	if (p > 0)
	{
		// must have control_points > degree
		if (p >= ncp)
			warning |= 0b0001;
		// if weights active, all must be > 0.0
		if (r != 0)
			for (int i = 0; i < ncp; i++)
				if (W[i] <= 0.0)
					warning |= 0b0010;
		// only '0' or '1' accepted for 'umethod' currently
		if (umethod < 0 || umethod > 1)
			warning |= 0b0100;
	}
	return warning;
}


NURBS create_nurbs (int n, int p, int r, int umethod, int upts,
	int max_cp, int max_deg, int max_upts, int max_fpts)
{
	NURBS a = (NURBS)malloc0 (sizeof(nurbs));
	
	a->n = n;
	a->p = p;
	a->r = r;
	a->umethod = umethod;
	a->upts = upts;
	a->fpts = 0;			// 'fpts' is not known at create time.
	a->max_cp = max_cp;
	a->max_p = max_deg;
	a->max_upts = max_upts;
	a->max_fpts = max_fpts;
	a->max_knots = a->max_cp + a->max_p + 1;

	a->U    = (double*)malloc0 (a->max_knots  * sizeof(double));
	a->CP   = (double*)malloc0 (a->max_cp * 2 * sizeof(double));
	a->W    = (double*)malloc0 (a->max_cp     * sizeof(double));
	a->Xs   = (double*)malloc0 (a->max_upts   * sizeof(double));
	a->Ys   = (double*)malloc0 (a->max_upts   * sizeof(double));
	a->Uout = (double*)malloc0 (a->max_upts   * sizeof(double));
	a->Xf   = (double*)malloc0 (a->max_fpts   * sizeof(double));
	a->Yf   = (double*)malloc0 (a->max_fpts   * sizeof(double));

	for (int i = 0; i < a->max_cp; i++)
		a->W[i] = 1.0;
	return a;
}

void destroy_nurbs (NURBS a)
{
	_aligned_free (a->Yf);
	_aligned_free (a->Xf);
	_aligned_free (a->Uout);
	_aligned_free (a->Ys);
	_aligned_free (a->Xs);
	_aligned_free (a->W);
	_aligned_free (a->CP);
	_aligned_free (a->U);
	_aligned_free (a);
}
