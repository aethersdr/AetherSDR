/*  extrapolate.c

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

/* This file was developed with the assistance of Claude (Anthropic),
 * an AI assistant, including both earlier implementations and the
 * subsequent clean-room rewrite.  The earlier AI-generated
 * implementations were replaced in their entirety by the clean-room
 * rewrite described below, which was derived from published algorithm
 * descriptions without reference to any prior implementation.
 */

 /* Clean-room implementation of LOWESS smoothing derived from:
  *   Cleveland, W.S., "Robust Locally Weighted Regression and
  *   Smoothing Scatterplots", J. Amer. Stat. Assoc., 74:829-836, 1979.
  * No GPL-encumbered or proprietary source code was consulted
  * in the writing of this file.
  */


#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "extrapolate.h"
 
#define LOWESS_BANDWIDTH   0.15
#define LOWESS_ROBUST_ITER 3 
#define TAIL_X_MIN         0.90 
#define AGREE_TOL_FRAC     0.05

static double tricube(double u)
{
    double au = fabs(u);
    if (au >= 1.0) return 0.0;
    double t = 1.0 - au * au * au;
    return t * t * t;
}

static double bisquare(double u)
{
    double au = fabs(u);
    if (au >= 1.0) return 0.0;
    double t = 1.0 - au * au;
    return t * t;
}

static double median_of(double *arr, int n)
{
     
    for (int i = 1; i < n; i++) 
    {
        double key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) { arr[j+1] = arr[j]; j--; }
        arr[j+1] = key;
    }
    if (n % 2 == 1) return arr[n/2];
    return 0.5 * (arr[n/2 - 1] + arr[n/2]);
}

static double wls_degree1(const double *xv, const double *yv,
                           const double *wv, int n, double x_eval)
{
    double s0=0, s1=0, s2=0, t0=0, t1=0;
    for (int i = 0; i < n; i++) 
    {
        double w = wv[i];
        s0 += w;
        s1 += w * xv[i];
        s2 += w * xv[i] * xv[i];
        t0 += w * yv[i];
        t1 += w * xv[i] * yv[i];
    }
    double det = s0*s2 - s1*s1;
    if (fabs(det) < 1e-15) return t0 / (s0 + 1e-30);
    double c0 = (s2*t0 - s1*t1) / det;
    double c1 = (s0*t1 - s1*t0) / det;
    return c0 + c1 * x_eval;
}
 
static int gauss_solve(double *A, double *b, double *x, int sz)
{
     
    for (int col = 0; col < sz; col++) {
         
        int pivot = col;
        double best = fabs(A[col*sz + col]);
        for (int row = col+1; row < sz; row++) 
        {
            double v = fabs(A[row*sz + col]);
            if (v > best) { best = v; pivot = row; }
        }
        if (best < 1e-15) return -1;
         
        if (pivot != col) 
        {
            for (int k = 0; k < sz; k++) 
            {
                double tmp = A[col*sz+k]; A[col*sz+k] = A[pivot*sz+k];
                A[pivot*sz+k] = tmp;
            }
            double tmp = b[col]; b[col] = b[pivot]; b[pivot] = tmp;
        }
         
        for (int row = col+1; row < sz; row++) 
        {
            double factor = A[row*sz + col] / A[col*sz + col];
            for (int k = col; k < sz; k++)
                A[row*sz+k] -= factor * A[col*sz+k];
            b[row] -= factor * b[col];
        }
    }
     
    for (int row = sz-1; row >= 0; row--) 
    {
        x[row] = b[row];
        for (int k = row+1; k < sz; k++)
            x[row] -= A[row*sz+k] * x[k];
        x[row] /= A[row*sz+row];
    }
    return 0;
}

static double wls_poly(const double *xv, const double *yv,
                        const double *wv, int n, int deg, double x_eval)
{
    int sz = deg + 1;
    double A[16] = {0};   
    double b[4]  = {0};
    for (int i = 0; i < n; i++) 
    {
        double w  = wv[i];
        double xp[4]; xp[0] = 1.0;
        for (int k = 1; k < sz; k++) xp[k] = xp[k-1] * xv[i];
        for (int r = 0; r < sz; r++) 
        {
            b[r] += w * yv[i] * xp[r];
            for (int c = 0; c < sz; c++)
                A[r*sz + c] += w * xp[r] * xp[c];
        }
    }
    double coef[4] = {0};
    if (gauss_solve(A, b, coef, sz) != 0)
    { 
        return wls_degree1(xv, yv, wv, n, x_eval);
    }

    double result = 0.0, xp = 1.0;
    for (int k = 0; k < sz; k++) { result += coef[k] * xp; xp *= x_eval; }
    return result;
}
 
#define BISQUARE_SCALE 6.0
 
static void lowess(const double *xs, const double *ys, int n,
                   double bandwidth, int robust_iters, double *smoothed)
{
    int k = (int)ceil(bandwidth * (double)n);
    if (k < 2) k = 2;
    if (k > n) k = n;
    double *rw    = (double *)malloc(n * sizeof(double));
    double *resid = (double *)malloc(n * sizeof(double));
    if (!rw || !resid) 
    {
        memcpy(smoothed, ys, n * sizeof(double));
        free(rw);
        free(resid);
        return;
    }
    for (int i = 0; i < n; i++) rw[i] = 1.0;
    for (int iter = 0; iter <= robust_iters; iter++) 
    { 
        for (int i = 0; i < n; i++) {
            int lo = i, hi = i, count = 1;
            while (count < k) 
            {
                double d_lo = (lo > 0)   ? xs[i] - xs[lo - 1] : 1e300;
                double d_hi = (hi < n-1) ? xs[hi + 1] - xs[i] : 1e300;
                if (d_lo <= d_hi) lo--; else hi++;
                count++;
            }
            double dmax = fmax(xs[i] - xs[lo], xs[hi] - xs[i]);
            if (dmax < 1e-12) dmax = 1e-12;
            double S0=0.0, S1=0.0, S2=0.0, T0=0.0, T1=0.0;
            for (int j = lo; j <= hi; j++) 
            {
                double u = fabs(xs[j] - xs[i]) / dmax;
                double w = tricube(u) * rw[j];
                S0 += w;
                S1 += w * xs[j];
                S2 += w * xs[j] * xs[j];
                T0 += w * ys[j];
                T1 += w * xs[j] * ys[j];
            }
            double det = S0 * S2 - S1 * S1;
            if (fabs(det) < 1e-15) 
            {
                smoothed[i] = (S0 > 0.0) ? T0 / S0 : ys[i];
            } 
            else 
            {
                double c0 = (S2 * T0 - S1 * T1) / det;
                double c1 = (S0 * T1 - S1 * T0) / det;
                smoothed[i] = c0 + c1 * xs[i];
            }
        }
        if (iter == robust_iters) break;
        for (int i = 0; i < n; i++)
            resid[i] = fabs(ys[i] - smoothed[i]);
        double *resid_sorted = (double *)malloc(n * sizeof(double));
        if (!resid_sorted) continue;
        memcpy(resid_sorted, resid, n * sizeof(double));
        double med = median_of(resid_sorted, n);
        free(resid_sorted);
        double scale = fmax(BISQUARE_SCALE * med, 1e-10);
        for (int i = 0; i < n; i++)
            rw[i] = bisquare(resid[i] / scale);
    }
    free(rw);
    free(resid);
}
 
typedef struct { double x, y; } Point;

static int cmp_point_x(const void *a, const void *b)
{
    double dx = ((const Point *)a)->x - ((const Point *)b)->x;
    return (dx > 0) - (dx < 0);
}

ExtrapolationResult extrapolate_y_at_1(const double *x, const double *y, int n)
{
    ExtrapolationResult result = {0.5, EXTRAP_LOW_CONFIDENCE, 0.5, 0.5, 0.5};

    if (n < 4) 
    {
        result.y_at_1 = 1.0;
        return result;
    } 
    Point *pts = (Point *)malloc(n * sizeof(Point));
    if (!pts) return result;
    for (int i = 0; i < n; i++) { pts[i].x = x[i]; pts[i].y = y[i]; }
    for (int i = 0; i < n; i++) 
    {
        if (pts[i].x < 0.0) pts[i].x = 0.0;   
        if (pts[i].x > 1.0) pts[i].x = 1.0;   
    }
    qsort(pts, n, sizeof(Point), cmp_point_x);
    double *xs = (double *)malloc(n * sizeof(double));
    double *ys = (double *)malloc(n * sizeof(double));
    double *sm = (double *)malloc(n * sizeof(double));
    if (!xs || !ys || !sm) 
    {
        free(pts); free(xs); free(ys); free(sm);
        return result;
    }
    for (int i = 0; i < n; i++) { xs[i] = pts[i].x; ys[i] = pts[i].y; }
    free(pts); 
    lowess(xs, ys, n, LOWESS_BANDWIDTH, LOWESS_ROBUST_ITER, sm); 
    int tail_n = 0;
    for (int i = 0; i < n; i++)
        if (xs[i] >= TAIL_X_MIN) tail_n++;
    if (tail_n < 4) 
    {
        tail_n = n / 10;
        if (tail_n < 4) tail_n = 4;
        if (tail_n > n) tail_n = n;
    }
    int tail_start = n - tail_n;
    const double *tx = xs + tail_start;    
    const double *ty = sm + tail_start;     
    double *tw = (double *)malloc(tail_n * sizeof(double));
    if (!tw) 
    {
        free(xs); free(ys); free(sm);
        return result;
    }
    double x_min_tail = tx[0];
    double x_range    = 1.0 - x_min_tail;
    if (x_range < 1e-9) x_range = 1e-9;
    for (int i = 0; i < tail_n; i++) 
    {
        double rel = (tx[i] - x_min_tail) / x_range;   
        tw[i] = 0.1 + 0.9 * rel;   
    }
    double y1 = wls_poly(tx, ty, tw, tail_n, 1, 1.0);
    double y2 = wls_poly(tx, ty, tw, tail_n, 2, 1.0);
    double y3 = wls_poly(tx, ty, tw, tail_n, 3, 1.0);
    double ty_min = ty[0], ty_max = ty[0];
    for (int i = 1; i < tail_n; i++) 
    {
        if (ty[i] < ty_min) ty_min = ty[i];
        if (ty[i] > ty_max) ty_max = ty[i];
    }
    double tail_y_range = ty_max - ty_min;
    if (tail_y_range < 1e-6) tail_y_range = 1e-6;
    double agree_tol = AGREE_TOL_FRAC * tail_y_range;
    if (agree_tol < 0.005) agree_tol = 0.005;
    double lo = fmin(fmin(y1, y2), y3);
    double hi = fmax(fmax(y1, y2), y3);
    double spread = hi - lo;
    result.y_linear    = y1;
    result.y_quadratic = y2;
    result.y_cubic     = y3;
    if (spread <= agree_tol) 
    {
        result.y_at_1      = y2;    
        result.confidence  = EXTRAP_CONFIDENT;
    } 
    else 
    {
        result.y_at_1      = y1;    
        result.confidence  = EXTRAP_LOW_CONFIDENCE;
    }
    free(xs); free(ys); free(sm); free(tw);
    return result;
}

ExtrapolationResult extrapolate_y_at_0(const double *x, const double *y,
                                        int n,
                                        double x_lo,
                                        double x_head_max)
{
    ExtrapolationResult result = {1.0, EXTRAP_LOW_CONFIDENCE, 1.0, 1.0, 1.0};
    if (n < 4) 
    {
        return result;
    }
    if (x_lo < 0.0)       x_lo = 0.0;
    if (x_head_max <= 0.0) x_head_max = 0.15;
    if (x_head_max >  1.0) x_head_max = 1.0;
    if (x_lo >= x_head_max) x_lo = 0.0;    
    Point *pts = (Point *)malloc(n * sizeof(Point));
    if (!pts) return result;
    int n_filt = 0;
    for (int i = 0; i < n; i++) 
    {
        double xi = x[i];
        if (xi < x_lo) continue;               
        if (xi < 0.0)  xi = 0.0;
        if (xi > 1.0)  xi = 1.0;
        pts[n_filt].x = xi;
        pts[n_filt].y = y[i];
        n_filt++;
    }
    if (n_filt < 4) 
    {
        free(pts);
        return result;
    }
    qsort(pts, n_filt, sizeof(Point), cmp_point_x);
    double *xs = (double *)malloc(n_filt * sizeof(double));
    double *ys = (double *)malloc(n_filt * sizeof(double));
    double *sm = (double *)malloc(n_filt * sizeof(double));
    if (!xs || !ys || !sm) 
    {
        free(pts); free(xs); free(ys); free(sm);
        return result;
    }
    for (int i = 0; i < n_filt; i++) { xs[i] = pts[i].x; ys[i] = pts[i].y; }
    free(pts);
    lowess(xs, ys, n_filt, LOWESS_BANDWIDTH, LOWESS_ROBUST_ITER, sm);
    int head_n = 0;
    for (int i = 0; i < n_filt; i++)
        if (xs[i] <= x_head_max) head_n++;
    if (head_n < 4) 
    {
        head_n = n_filt / 10;
        if (head_n < 4)  head_n = 4;
        if (head_n > n_filt) head_n = n_filt;
    }
    const double *hx = xs;         
    const double *hy = sm;         
    double *hw = (double *)malloc(head_n * sizeof(double));
    if (!hw) 
    {
        free(xs); free(ys); free(sm);
        return result;
    }
    double x_min_head = hx[0];
    double x_span     = x_head_max - x_min_head;
    if (x_span < 1e-9) x_span = 1e-9;
    for (int i = 0; i < head_n; i++) 
    { 
        double rel = 1.0 - (hx[i] - x_min_head) / x_span;
        if (rel < 0.0) rel = 0.0;
        if (rel > 1.0) rel = 1.0;
        hw[i] = 0.1 + 0.9 * rel;    
    }
    double y1 = wls_poly(hx, hy, hw, head_n, 1, 0.0);
    double y2 = wls_poly(hx, hy, hw, head_n, 2, 0.0);
    double y3 = wls_poly(hx, hy, hw, head_n, 3, 0.0); 
    double hy_min = hy[0], hy_max = hy[0];
    for (int i = 1; i < head_n; i++) 
    {
        if (hy[i] < hy_min) hy_min = hy[i];
        if (hy[i] > hy_max) hy_max = hy[i];
    }
    double head_y_range = hy_max - hy_min;
    if (head_y_range < 1e-6) head_y_range = 1e-6;
    double agree_tol = AGREE_TOL_FRAC * head_y_range;
    if (agree_tol < 0.005) agree_tol = 0.005;
    double lo     = fmin(fmin(y1, y2), y3);
    double hi_val = fmax(fmax(y1, y2), y3);
    double spread = hi_val - lo;
    result.y_linear    = y1;
    result.y_quadratic = y2;
    result.y_cubic     = y3;
    if (spread <= agree_tol) 
    {
        result.y_at_1     = y2;    
        result.confidence = EXTRAP_CONFIDENT;
    } 
    else 
    {
        result.y_at_1     = y1;    
        result.confidence = EXTRAP_LOW_CONFIDENCE;
    }
    free(xs); free(ys); free(sm); free(hw);
    return result;
}


