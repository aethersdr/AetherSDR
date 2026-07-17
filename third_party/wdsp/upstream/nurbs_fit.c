/*  nurbs_fit.c

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

 /* Clean-room implementation derived from algorithms published in:
  *   Piegl, L. and Tiller, W., "The NURBS Book", 2nd ed.,
  *   Springer, 1997.  (find_span, basis_funs, least-squares fitting)
  *   de Boor, C., "A Practical Guide to Splines", Springer, 1978.
  * No GPL-encumbered or proprietary source code was consulted
  * in the writing of this file.
  */


#define _CRT_SECURE_NO_WARNINGS
#include "nurbs_fit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>

#ifndef NAN
#  define NAN (0.0/0.0)
#endif

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "nurbs_fit: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

static void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n, sz);
    if (!p) { fprintf(stderr, "nurbs_fit: out of memory\n"); exit(1); }
    return p;
}

static double sq(double x)                  { return x * x; }

static double dist2(NF_Point2 a, NF_Point2 b) { return sq(a.x-b.x)+sq(a.y-b.y); }

void nf_default_config(NF_Config *cfg)
{
    cfg->degree             = 3;
    cfg->n_ctrl             = 0;       
    cfg->n_ctrl_max         = 200;
    cfg->ordering_mode      = NF_ORDER_AUTO;
    cfg->spearman_threshold = 0.85;
    cfg->nn_search_k        = 10; 
    cfg->outlier_iters      = 2;
    cfg->outlier_sigma      = 3.0;
    cfg->outlier_min_fraction = 0.5; 
    cfg->pin_start          = 0;
    cfg->pin_end            = 0;
    cfg->start_pt.x = cfg->start_pt.y = 0.0;
    cfg->end_pt.x   = cfg->end_pt.y   = 0.0;
    cfg->adaptive_iters     = 5;
    cfg->adaptive_threshold = 2.0;
    cfg->cv_fraction        = 0.1;
    cfg->cv_overfit_ratio   = 1.5;
    cfg->cv_fatal_ratio     = 10.0;
    cfg->min_pts_per_ctrl   = 20;
    cfg->x_weight_x0  = 0.0;     
    cfg->x_weight_min = 0.1;
    cfg->local_outlier_iters = 2;
    cfg->local_outlier_sigma = 4.0;
    cfg->local_outlier_bands = 20;
    cfg->fold_detect  = 1;
    cfg->y_min              = 0.0;
    cfg->y_max              = 0.0;    
    cfg->pre_filter_x_min   = 0.0;   
    cfg->pre_filter_y_max   = 0.0;    
    cfg->direct_n_segments        = 22;    
    cfg->direct_monotone_x_start  = 0.8;    
    cfg->reparam_iters      = 2; 
    cfg->irls_iters         = 2;
    cfg->irls_epsilon       = 1e-6;
}
 
static int cmp_by_x(const void *a, const void *b)
{
    double xa = ((const NF_Point2*)a)->x;
    double xb = ((const NF_Point2*)b)->x;
    return (xa > xb) - (xa < xb);
}

static int *order_points_nn(const NF_Point2 *pts, int n)
{
    int   *order   = (int *)xmalloc(n * sizeof(int));
    char  *visited = (char*)xcalloc(n, 1);
    int start = 0;
    for (int i = 1; i < n; i++)
        if (pts[i].x < pts[start].x) start = i;
    order[0]       = start;
    visited[start] = 1; 
    for (int step = 1; step < n; step++) {
        int    cur    = order[step-1];
        double best_d = DBL_MAX;
        int    best_j = -1;
        for (int j = 0; j < n; j++) {
            if (visited[j]) continue;
            double d = dist2(pts[cur], pts[j]);
            if (d < best_d) { best_d = d; best_j = j; }
        }
        order[step]     = best_j;
        visited[best_j] = 1;
    }
    free(visited);
    for (int pass = 0; pass < 5; pass++) {
        int improved = 0;
        for (int i = 0; i < n - 2; i++) {
            for (int k = i + 2; k < n - 1; k++) {
                double d_old = dist2(pts[order[i]],   pts[order[i+1]])
                             + dist2(pts[order[k]],   pts[order[k+1]]);
                double d_new = dist2(pts[order[i]],   pts[order[k]])
                             + dist2(pts[order[i+1]], pts[order[k+1]]);
                if (d_new < d_old - 1e-14) {
                     
                    int lo = i+1, hi = k;
                    while (lo < hi) {
                        int tmp = order[lo]; order[lo] = order[hi];
                        order[hi] = tmp; lo++; hi--;
                    }
                    improved = 1;
                }
            }
        }
        if (!improved) break;
    }
    return order;
}

#define NF_SPEARMAN_SAMPLE 512

double nf_spearman(const NF_Point2 *pts, int n)
{
    int m = (n <= NF_SPEARMAN_SAMPLE) ? n : NF_SPEARMAN_SAMPLE;

    double *xs = (double*)xmalloc(m * sizeof(double));
    double *ys = (double*)xmalloc(m * sizeof(double));
    int    *ix = (int*)   xmalloc(m * sizeof(int));
    int    *iy = (int*)   xmalloc(m * sizeof(int));
    double *rx = (double*)xmalloc(m * sizeof(double));
    double *ry = (double*)xmalloc(m * sizeof(double));

     
    for (int i = 0; i < m; i++) {
        int src = (n <= NF_SPEARMAN_SAMPLE) ? i : (int)((long)i * n / m);
        xs[i] = pts[src].x;
        ys[i] = pts[src].y;
        ix[i] = iy[i] = i;
    }
    for (int i = 1; i < m; i++) {
        int key = ix[i], j = i-1;
        while (j >= 0 && xs[ix[j]] > xs[key]) { ix[j+1] = ix[j]; j--; }
        ix[j+1] = key;
    }
    for (int i = 0; i < m; i++) rx[ix[i]] = (double)i; 
    for (int i = 1; i < m; i++) {
        int key = iy[i], j = i-1;
        while (j >= 0 && ys[iy[j]] > ys[key]) { iy[j+1] = iy[j]; j--; }
        iy[j+1] = key;
    }
    for (int i = 0; i < m; i++) ry[iy[i]] = (double)i; 
    double mean = (m - 1) / 2.0;
    double num = 0.0, dx2 = 0.0, dy2 = 0.0;
    for (int i = 0; i < m; i++) {
        double drx = rx[i] - mean, dry = ry[i] - mean;
        num += drx * dry;
        dx2 += drx * drx;
        dy2 += dry * dry;
    }
    double rho = (dx2 > 0 && dy2 > 0) ? num / sqrt(dx2 * dy2) : 0.0;
    free(xs); free(ys); free(ix); free(iy); free(rx); free(ry);
    return rho;
}

static double *centripetal_parameterise(const NF_Point2 *ordered, int n)
{
    double *t = (double*)xmalloc(n * sizeof(double));
    t[0] = 0.0;
    double total = 0.0;
    for (int i = 1; i < n; i++) {
        


 
        total += sqrt(sqrt(dist2(ordered[i], ordered[i-1])));
        t[i] = total;
    }
    if (total < DBL_EPSILON) total = 1.0;
    for (int i = 0; i < n; i++) t[i] /= total;
    return t;
}

static double *make_knots(const double *t, int m, int n, int p)
{
    int     n_knots = n + p + 1;
    double *U       = (double*)xcalloc(n_knots, sizeof(double));

     
    for (int i = 0; i <= p; i++) { U[i] = 0.0; U[n_knots-1-i] = 1.0; }
    double d = (double)m / (double)(n - p);
    for (int j = 1; j <= n - p - 1; j++) {
        double sum = 0.0;
        int    i0  = (int)(j * d);
        for (int l = i0; l < i0 + p; l++)
            sum += t[(l < m) ? l : m-1];
        U[p + j] = sum / p;
    }
    return U;
}
 
static int find_span(int n, int p, double t, const double *U)
{
    if (t >= U[n + 1]) return n;
    if (t <= U[p])     return p;
    int lo = p, hi = n + 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (t < U[mid]) hi = mid;
        else            lo = mid;
    }
    return lo;
}
 
static void basis_funs(int span, double t, int p,
                       const double *U, double *N)
{
    double left[64], right[64];
    N[0] = 1.0;    
    for (int j = 1; j <= p; j++) {
        left[j]  = t - U[span + 1 - j];    
        right[j] = U[span + j] - t;        
        double carry = 0.0;
        for (int r = 0; r < j; r++) {
            double denom  = right[r + 1] + left[j - r];
            double alpha  = (denom > DBL_EPSILON) ? left[j - r] / denom : 0.0;
            double prev_r = N[r];
            N[r]  = carry + (1.0 - alpha) * prev_r;
            carry = alpha * prev_r;    
        }
        N[j] = carry;    
    }
}

typedef struct { int m, n; double *data; } Mat;

static Mat  mat_alloc(int m, int n) {
    Mat A; A.m=m; A.n=n;
    A.data=(double*)xcalloc((size_t)m*n,sizeof(double)); return A; }

static void mat_free(Mat *A) { free(A->data); A->data=NULL; }

#define MAT(A,r,c) ((A).data[(size_t)(c)*(A).m+(r)])

static Mat build_collocation(const double *t_params, int m,
                              const double *U, int n_ctrl, int p)
{
    Mat    N     = mat_alloc(m, n_ctrl);
    double basis[64];
    for (int i = 0; i < m; i++) {
        double ti = t_params[i];
        if (ti < 0.0) ti = 0.0;
        if (ti > 1.0) ti = 1.0;
        int span = find_span(n_ctrl-1, p, ti, U);
        basis_funs(span, ti, p, U, basis);
        for (int j = 0; j <= p; j++)
            MAT(N, i, span-p+j) = basis[j];
    }
    return N;
}

static int cholesky(double *L, int n)
{
    for (int j = 0; j < n; j++) {
        double s = L[j*n+j];
        for (int k = 0; k < j; k++) s -= L[j*n+k]*L[j*n+k];
        if (s <= 0.0) return -1;
        L[j*n+j] = sqrt(s);
        for (int i = j+1; i < n; i++) {
            double t = L[i*n+j];
            for (int k = 0; k < j; k++) t -= L[i*n+k]*L[j*n+k];
            L[i*n+j] = t / L[j*n+j];
            L[j*n+i] = 0.0;
        }
    }
    return 0;
}

static void chol_solve(const double *L, int n, double *b)
{
    for (int i = 0;   i < n;  i++) {
        for (int k=0; k<i; k++) b[i] -= L[i*n+k]*b[k];
        b[i] /= L[i*n+i];
    }
    for (int i = n-1; i >= 0; i--) {
        for (int k=i+1; k<n; k++) b[i] -= L[k*n+i]*b[k];
        b[i] /= L[i*n+i];
    }
}
 
static void ls_solve_with_cond(const Mat *A, const double *b,
                                double *x, double *cond_out)
{
    int m = A->m, n = A->n;

    double *AtA = (double*)xcalloc((size_t)n*n, sizeof(double));
    double *Atb = (double*)xcalloc(n, sizeof(double));
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            double Aij = MAT(*A,i,j);
            if (Aij == 0.0) continue;
            Atb[j] += Aij * b[i];
            for (int k = 0; k <= j; k++)
                AtA[j*n+k] += Aij * MAT(*A,i,k);
        }
    }
    for (int j = 0; j < n; j++)
        for (int k = j+1; k < n; k++)
            AtA[j*n+k] = AtA[k*n+j];
    double *L = (double*)xmalloc((size_t)n*n * sizeof(double));
    memcpy(L, AtA, (size_t)n*n * sizeof(double));
    if (cholesky(L, n) != 0) {
        double lambda = 0.0;
        for (int i = 0; i < n; i++) lambda += AtA[i*n+i];
        lambda = (lambda / n) * 1e-8;
        if (lambda < 1e-30) lambda = 1e-10;
        memcpy(L, AtA, (size_t)n*n * sizeof(double));
        for (int i = 0; i < n; i++) L[i*n+i] += lambda;
        if (cholesky(L, n) != 0) {
             
            for (int i = 0; i < n; i++)
                x[i] = (fabs(AtA[i*n+i]) > 1e-30) ? Atb[i]/AtA[i*n+i] : 0.0;
            if (cond_out) *cond_out = 1e16;    
            free(L); free(AtA); free(Atb);
            return;
        }
    }
    if (cond_out) {
        double dmax = 0.0, dmin = DBL_MAX;
        for (int i = 0; i < n; i++) {
            double d = fabs(L[i*n+i]);
            if (d > dmax) dmax = d;
            if (d < dmin) dmin = d;
        }
        *cond_out = (dmin > 1e-30) ? sq(dmax/dmin) : 1e16;
    }
    memcpy(x, Atb, n * sizeof(double));
    chol_solve(L, n, x);
    free(L); free(AtA); free(Atb);
}
 
static void ls_solve(const Mat *A, const double *b, double *x)
{
    ls_solve_with_cond(A, b, x, NULL);
}

static int fit_pass(const NF_Point2 *pts_ordered, int m,
                    const double    *t_params,
                    const double    *w_data,       
                    const double    *U,
                    int              n_ctrl,
                    int              p,
                    const NF_Config *cfg,
                    NF_Curve        *out,
                    double          *cond_out)
{
    Mat N = build_collocation(t_params, m, U, n_ctrl, p);
    double *bx = (double*)xmalloc(m * sizeof(double));
    double *by = (double*)xmalloc(m * sizeof(double));
    for (int i = 0; i < m; i++) {
        double wi = w_data ? w_data[i] : 1.0;
        if (cfg && cfg->x_weight_x0 > 0.0) {
            double xi = pts_ordered[i].x;
            double wx;
            if (xi >= cfg->x_weight_x0) {
                wx = 1.0;
            } else if (xi <= 0.0) {
                wx = cfg->x_weight_min;
            } else {
                wx = cfg->x_weight_min
                   + (1.0 - cfg->x_weight_min) * (xi / cfg->x_weight_x0);
            }
            wi *= wx;
        }
        for (int j = 0; j < n_ctrl; j++) MAT(N,i,j) *= wi;
        bx[i] = pts_ordered[i].x * wi;
        by[i] = pts_ordered[i].y * wi;
    }
    int first_free = 0, last_free = n_ctrl - 1;
    if (cfg && cfg->pin_start) {
        double sx = cfg->start_pt.x, sy = cfg->start_pt.y;
        for (int i = 0; i < m; i++) {
            bx[i] -= MAT(N,i,0) * sx;
            by[i] -= MAT(N,i,0) * sy;
            MAT(N,i,0) = 0.0;
        }
        first_free = 1;
    }
    if (cfg && cfg->pin_end) {
        double ex = cfg->end_pt.x, ey = cfg->end_pt.y;
        for (int i = 0; i < m; i++) {
            bx[i] -= MAT(N,i,n_ctrl-1) * ex;
            by[i] -= MAT(N,i,n_ctrl-1) * ey;
            MAT(N,i,n_ctrl-1) = 0.0;
        }
        last_free = n_ctrl - 2;
    }
    int n_free = last_free - first_free + 1;
    double *cx = (double*)xcalloc(n_ctrl, sizeof(double));
    double *cy = (double*)xcalloc(n_ctrl, sizeof(double));
    if (n_free > 0) {
        Mat Nfree = mat_alloc(m, n_free);
        for (int j = 0; j < n_free; j++)
            for (int i = 0; i < m; i++)
                MAT(Nfree,i,j) = MAT(N,i,first_free+j);
        double *cxf = (double*)xcalloc(n_free, sizeof(double));
        double *cyf = (double*)xcalloc(n_free, sizeof(double));
        ls_solve_with_cond(&Nfree, bx, cxf, cond_out);
        ls_solve          (&Nfree, by, cyf);
        for (int j = 0; j < n_free; j++) {
            cx[first_free+j] = cxf[j];
            cy[first_free+j] = cyf[j];
        }
        free(cxf); free(cyf);
        mat_free(&Nfree);
    } else if (cond_out) {
        *cond_out = 1.0;    
    } 
    if (cfg && cfg->pin_start) {
        cx[0] = cfg->start_pt.x;
        cy[0] = cfg->start_pt.y;
    }
    if (cfg && cfg->pin_end) {
        cx[n_ctrl-1] = cfg->end_pt.x;
        cy[n_ctrl-1] = cfg->end_pt.y;
    } 
    for (int i = 0; i < n_ctrl; i++) {
        out->ctrl_wx[i] = cx[i];
        out->ctrl_wy[i] = cy[i];
        out->weights[i] = 1.0;
    }
    free(bx); free(by); free(cx); free(cy);
    mat_free(&N);
    return 0;
}

NF_Point2 nf_eval(const NF_Curve *c, double t)
{
    int p = c->degree, n = c->n_ctrl;
    if (t <= 0.0) { NF_Point2 r={c->ctrl_wx[0],   c->ctrl_wy[0]};   return r; }
    if (t >= 1.0) { NF_Point2 r={c->ctrl_wx[n-1], c->ctrl_wy[n-1]}; return r; }

    double basis[64];
    int span = find_span(n-1, p, t, c->knots);
    basis_funs(span, t, p, c->knots, basis);

    double wx=0, wy=0, ww=0;
    for (int j = 0; j <= p; j++) {
        int idx = span-p+j;
        wx += basis[j]*c->ctrl_wx[idx];
        wy += basis[j]*c->ctrl_wy[idx];
        ww += basis[j]*c->weights[idx];
    }
    NF_Point2 r = { wx/ww, wy/ww };
    return r;
}

static NF_Point2 nf_deriv(const NF_Curve *c, double t)
{
    double h  = 1e-6;
    double t0 = t-h, t1 = t+h;
    if (t0 < 0.0) { t0=0.0; t1=2*h; }
    if (t1 > 1.0) { t1=1.0; t0=1.0-2*h; }
    NF_Point2 p0=nf_eval(c,t0), p1=nf_eval(c,t1);
    NF_Point2 d = { (p1.x-p0.x)/(2*h), (p1.y-p0.y)/(2*h) };
    return d;
}

static double compute_rms_params(const NF_Curve *c,
                                  const NF_Point2 *pts, int m,
                                  const double *t_params)
{
    double sse = 0.0;
    for (int i = 0; i < m; i++) {
        NF_Point2 ev = nf_eval(c, t_params[i]);
        sse += sq(ev.x-pts[i].x) + sq(ev.y-pts[i].y);
    }
    return sqrt(sse / m);
}
 
static int reparameterise_inplace(const NF_Curve *c,
                                   const NF_Point2 *ordered, int m,
                                   double *t_params)
{
     
    double *t_old = (double*)xmalloc(m * sizeof(double));
    memcpy(t_old, t_params, m * sizeof(double));
    double rms_before = compute_rms_params(c, ordered, m, t_params);
    for (int i = 0; i < m; i++) {
        double ti = t_params[i];
        for (int iter = 0; iter < 8; iter++) {
            NF_Point2 ev = nf_eval(c, ti);
            NF_Point2 dv = nf_deriv(c, ti);
            double dx = ev.x - ordered[i].x, dy = ev.y - ordered[i].y;
            double f  = dx*dv.x + dy*dv.y;
            double df = dv.x*dv.x + dv.y*dv.y;
            if (df < 1e-20) break;
            double step = f/df;
             
            if (step >  0.1) step =  0.1;
            if (step < -0.1) step = -0.1;
            double tnew = ti - step;
            if (tnew < 0.0) tnew = 0.0;
            if (tnew > 1.0) tnew = 1.0;
            if (fabs(tnew-ti) < 1e-10) break;
            ti = tnew;
        }
        t_params[i] = ti;
    }
    double rms_after = compute_rms_params(c, ordered, m, t_params);
    if (rms_after >= rms_before) {
        memcpy(t_params, t_old, m * sizeof(double));
        free(t_old);
        return 0;
    }
    free(t_old);
    return 1;
}
 
static void compute_segment_rms(const NF_Curve *c,
                                  const NF_Point2 *ordered, int m,
                                  const double *t_params,
                                  double *seg_rms, int *seg_count)
{
    int n = c->n_ctrl, p = c->degree;
    int n_segs = n - p;    
    double *seg_sse = (double*)xcalloc(n_segs, sizeof(double));
    for (int k = 0; k < n_segs; k++) { seg_rms[k]=0; seg_count[k]=0; }
    for (int i = 0; i < m; i++) {
        NF_Point2 ev  = nf_eval(c, t_params[i]);
        double    res = sq(ev.x-ordered[i].x) + sq(ev.y-ordered[i].y);
        int span = find_span(n-1, p, t_params[i], c->knots);
        int seg = span - p;
        if (seg < 0) seg = 0;
        if (seg >= n_segs) seg = n_segs-1;
        seg_sse[seg]   += res;
        seg_count[seg] += 1;
    }
    for (int k = 0; k < n_segs; k++) {
        seg_rms[k] = (seg_count[k] > 0)
                     ? sqrt(seg_sse[k] / seg_count[k])
                     : 0.0;
    }
    free(seg_sse);
}

static int compute_outlier_mask(const NF_Curve *c,
                                 const NF_Point2 *ordered, int m,
                                 const double *t_params,
                                 double outlier_sigma,
                                 char *inlier_mask)
{
    double *res = (double*)xmalloc(m * sizeof(double)); 
    double mean_res = 0.0;
    for (int i = 0; i < m; i++) {
        NF_Point2 ev = nf_eval(c, t_params[i]);
        res[i]    = sqrt(sq(ev.x-ordered[i].x) + sq(ev.y-ordered[i].y));
        mean_res += res[i];
    }
    mean_res /= m;
    double var = 0.0;
    for (int i = 0; i < m; i++) var += sq(res[i]-mean_res);
    double sigma = sqrt(var / m);
    double threshold = outlier_sigma * sigma;
    int n_outliers = 0;
    for (int i = 0; i < m; i++) {
        if (res[i] > threshold) {
            inlier_mask[i] = 0;
            n_outliers++;
        } else {
            inlier_mask[i] = 1;
        }
    }
    free(res);
    return n_outliers;
}
 
static double compute_cv_score(const NF_Curve *c,
                                const NF_Point2 *ordered,
                                int n_pts,
                                const double *t_params,
                                const char *cv_mask,
                                double outlier_sigma)
{
     
    double *res = (double*)xmalloc(n_pts * sizeof(double));
    int     n_cv = 0;
    double  mean_res = 0.0;
    for (int i = 0; i < n_pts; i++) {
        if (!cv_mask[i]) { res[i] = 0.0; continue; }
        NF_Point2 ev = nf_eval(c, t_params[i]);
        res[i] = sqrt(sq(ev.x - ordered[i].x) + sq(ev.y - ordered[i].y));
        mean_res += res[i];
        n_cv++;
    }
    if (n_cv == 0) { free(res); return 0.0; }
    mean_res /= n_cv;
    double threshold = DBL_MAX;
    if (outlier_sigma > 0.0) {
        double var = 0.0;
        for (int i = 0; i < n_pts; i++) {
            if (!cv_mask[i]) continue;
            var += sq(res[i] - mean_res);
        }
        double sigma = sqrt(var / n_cv);
        threshold = outlier_sigma * sigma;
    } 
    double sse = 0.0;
    int    cnt = 0;
    for (int i = 0; i < n_pts; i++) {
        if (!cv_mask[i]) continue;
        if (res[i] > threshold) continue;    
        sse += sq(res[i]);
        cnt++;
    }
    free(res);
    return (cnt > 0) ? sqrt(sse / cnt) : 0.0;
}

static double detect_fold_x_end(const NF_Curve *c)
{
    int    n_scan  = 2000;
    int    confirm = 10;    
    double tol     = 1e-4; 
    NF_Point2 p0 = nf_eval(c, 0.0);
    double x_start = p0.x;
    double x_min   = x_start;
    double t_min   = 0.0;
    int scan_end = (x_start < 0.0) ? n_scan/2 : n_scan/5;
    for (int i = 1; i <= scan_end; i++) {
        double t = (double)i / n_scan;
        NF_Point2 p = nf_eval(c, t);
        if (p.x < x_min) { x_min = p.x; t_min = t; }
    }
    if (x_start - x_min < tol && x_start >= 0.0) return -1.0;
    if (x_start < 0.0) x_min = x_start;
    int consec = 0;
    double t_fold_end = t_min;
    double x_prev = x_min;
    for (int i = (int)(t_min * n_scan) + 1; i <= n_scan; i++) {
        double t = (double)i / n_scan;
        NF_Point2 p = nf_eval(c, t);
        if (p.x > x_prev) {
            consec++;
            if (consec >= confirm) {
                for (int j = i - confirm; j <= i; j++) {
                    double tj = (double)j / n_scan;
                    NF_Point2 pj = nf_eval(c, tj);
                    if (pj.x >= x_min + tol) { t_fold_end = tj; break; }
                }
                return nf_eval(c, t_fold_end).x;
            }
        } else {
            consec = 0;
        }
        x_prev = p.x;
    }
    return x_min;
}
 
static int local_outlier_rejection(const NF_Curve *c,
                                    const NF_Point2 *ordered, int m,
                                    const double *t_params,
                                    char *inlier_mask,
                                    double local_sigma,
                                    int n_bands)
{
    if (local_sigma <= 0.0 || n_bands <= 0) return 0;
    double xlo = DBL_MAX, xhi = -DBL_MAX;
    for (int i = 0; i < m; i++) {
        if (!inlier_mask[i]) continue;
        if (ordered[i].x < xlo) xlo = ordered[i].x;
        if (ordered[i].x > xhi) xhi = ordered[i].x;
    }
    if (xhi <= xlo) return 0;
    double band_width = (xhi - xlo) / n_bands;
    if (band_width < 1e-10) return 0;
    int n_new_outliers = 0;
    for (int b = 0; b < n_bands; b++) {
        double x0 = xlo + b * band_width;
        double x1 = x0 + band_width;
        double *band_res = (double*)xmalloc(m * sizeof(double));
        int     n_band   = 0;
        int    *band_idx = (int*)xmalloc(m * sizeof(int));
        for (int i = 0; i < m; i++) {
            if (!inlier_mask[i]) continue;
            if (ordered[i].x < x0 || ordered[i].x >= x1) continue;
            NF_Point2 ev = nf_eval(c, t_params[i]);
            band_res[n_band] = fabs(ev.x - ordered[i].x) +
                               fabs(ev.y - ordered[i].y);
            band_idx[n_band] = i;
            n_band++;
        }
        if (n_band < 5) { free(band_res); free(band_idx); continue; }
        double *sorted = (double*)xmalloc(n_band * sizeof(double));
        memcpy(sorted, band_res, n_band * sizeof(double)); 
        for (int i = 1; i < n_band; i++) {
            double key = sorted[i]; int j = i-1;
            while (j >= 0 && sorted[j] > key) { sorted[j+1]=sorted[j]; j--; }
            sorted[j+1] = key;
        }
        double med = (n_band % 2 == 0)
                   ? 0.5*(sorted[n_band/2-1]+sorted[n_band/2])
                   : sorted[n_band/2];
        double *abs_dev = (double*)xmalloc(n_band * sizeof(double));
        for (int i = 0; i < n_band; i++)
            abs_dev[i] = fabs(band_res[i] - med);
        for (int i = 1; i < n_band; i++) {
            double key = abs_dev[i]; int j = i-1;
            while (j >= 0 && abs_dev[j] > key) { abs_dev[j+1]=abs_dev[j]; j--; }
            abs_dev[j+1] = key;
        }
        double mad = (n_band % 2 == 0)
                   ? 0.5*(abs_dev[n_band/2-1]+abs_dev[n_band/2])
                   : abs_dev[n_band/2];
        double local_std = 1.4826 * mad;
        if (local_std < 1e-10) { free(sorted); free(abs_dev); free(band_res); free(band_idx); continue; }
        double threshold = local_sigma * local_std;
        for (int i = 0; i < n_band; i++) {
            if (band_res[i] > threshold) {
                inlier_mask[band_idx[i]] = 0;
                n_new_outliers++;
            }
        }
        free(sorted); free(abs_dev); free(band_res); free(band_idx);
    }
    return n_new_outliers;
}

NF_Curve *nf_fit(const NF_Point2 *pts, int n_pts,
                 const NF_Config *cfg_in,
                 NF_FitResult    *result_out)
{
    NF_Config cfg;
    if (cfg_in) cfg = *cfg_in;
    else        nf_default_config(&cfg);
    int p = cfg.degree;
    if (p < 1) p = 1;
    if (p > 9) p = 9;
    NF_FitResult result;
    memset(&result, 0, sizeof(result));
    result.quality          = NF_FIT_OK;
    result.condition_number = 1.0;
    result.cv_score         = 0.0;
    result.fold_detected    = 0;
    result.fold_x_end       = 0.0;
    if (n_pts < p + 2) {
        fprintf(stderr, "nurbs_fit: need at least %d points for degree %d\n",
                p+2, p);
        result.quality |= NF_FIT_BAD_TOOFEW;
        if (result_out) *result_out = result;
        return NULL;
    }
    int eff_mode = cfg.ordering_mode;
    double rho = nf_spearman(pts, n_pts);
    result.spearman_rho = rho;
    if (eff_mode == NF_ORDER_AUTO)
        eff_mode = (fabs(rho) > cfg.spearman_threshold)
                   ? NF_ORDER_BY_X : NF_ORDER_NN;
    result.ordering_used = eff_mode;
    NF_Point2 *pts_filtered = NULL;
    int need_filter = (cfg.pre_filter_x_min > 0.0 || cfg.pre_filter_y_max > 0.0);
    if (need_filter) {
         
        int n_keep = 0;
        for (int i = 0; i < n_pts; i++) {
            if (cfg.pre_filter_x_min > 0.0 && pts[i].x < cfg.pre_filter_x_min) continue;
            if (cfg.pre_filter_y_max > 0.0 && pts[i].y > cfg.pre_filter_y_max) continue;
            n_keep++;
        }
        if (n_keep < n_pts) { 
            pts_filtered = (NF_Point2*)xmalloc(n_keep * sizeof(NF_Point2));
            int j = 0;
            for (int i = 0; i < n_pts; i++) {
                if (cfg.pre_filter_x_min > 0.0 && pts[i].x < cfg.pre_filter_x_min) continue;
                if (cfg.pre_filter_y_max > 0.0 && pts[i].y > cfg.pre_filter_y_max) continue;
                pts_filtered[j++] = pts[i];
            }
            pts   = pts_filtered;    
            n_pts = n_keep;
            result.quality |= NF_FIT_PRE_FILTERED;
        }
    }
    if (n_pts < p + 2) {
        fprintf(stderr,
                "nurbs_fit: pre-filter removed too many points "
                "(%d remain, need %d for degree %d)\n",
                n_pts, p+2, p);
        result.quality |= NF_FIT_BAD_TOOFEW;
        free(pts_filtered);
        if (result_out) *result_out = result;
        return NULL;
    }
    NF_Point2 *ordered = (NF_Point2*)xmalloc(n_pts * sizeof(NF_Point2));
    if (eff_mode == NF_ORDER_BY_X) {
        memcpy(ordered, pts, n_pts * sizeof(NF_Point2));
        qsort(ordered, n_pts, sizeof(NF_Point2), cmp_by_x);
    } else {
        int *order = order_points_nn(pts, n_pts);
        for (int i = 0; i < n_pts; i++) ordered[i] = pts[order[i]];
        free(order);
    }
    double *t_params = centripetal_parameterise(ordered, n_pts);
    int n = cfg.n_ctrl;
    if (n <= 0) {
        n = (int)(sqrt((double)n_pts) * 1.5);
        if (n < p+2)  n = p+2;
        if (n > 40)   n = 40;   
        if (cfg.min_pts_per_ctrl > 0) {
            int n_density = n_pts / cfg.min_pts_per_ctrl;
            if (n_density < p+2) n_density = p+2;
            if (n > n_density) n = n_density;
        }
    }
    if (n < p+2) n = p+2;
    result.n_ctrl_initial = n;
    NF_Curve *c = (NF_Curve*)xmalloc(sizeof(NF_Curve));
    c->degree  = p;
    c->n_ctrl  = n;
    c->knots   = make_knots(t_params, n_pts, n, p);
    c->ctrl_wx = (double*)xmalloc(n * sizeof(double));
    c->ctrl_wy = (double*)xmalloc(n * sizeof(double));
    c->weights = (double*)xmalloc(n * sizeof(double));
    char *inlier_mask = (char*)xmalloc(n_pts * sizeof(char));
    memset(inlier_mask, 1, n_pts);
    int n_inliers = n_pts;
    char *cv_mask = (char*)xcalloc(n_pts, 1);    
    int   n_cv    = 0;
    if (cfg.cv_fraction > 0.0 && cfg.cv_fraction < 1.0) {
        int stride = (int)(1.0 / cfg.cv_fraction + 0.5);
        if (stride < 2) stride = 2;
        for (int i = 0; i < n_pts; i += stride) {
            cv_mask[i] = 1;
            n_cv++;
        }
    }
    double cond = 1.0;
    fit_pass(ordered, n_pts, t_params, NULL, c->knots, n, p, &cfg, c, &cond);
    result.condition_number = cond;
    int total_outliers = 0;
    for (int out_pass = 0;
         out_pass < cfg.outlier_iters && n_inliers > p+2;
         out_pass++) {
        int n_flagged = compute_outlier_mask(c, ordered, n_pts, t_params,
                                              cfg.outlier_sigma, inlier_mask);
        for (int i = 0; i < n_pts; i++)
            if (cv_mask[i]) inlier_mask[i] = 0;
        n_inliers = 0;
        for (int i = 0; i < n_pts; i++)
            if (inlier_mask[i]) n_inliers++;
        if (n_flagged == 0) break;    
        if (n_inliers < (int)(n_pts * cfg.outlier_min_fraction)) {
            result.quality |= NF_FIT_BAD_TOOFEW; 
            memset(inlier_mask, 1, n_pts);
            for (int i = 0; i < n_pts; i++)
                if (cv_mask[i]) inlier_mask[i] = 0;
            n_inliers = n_pts - n_cv;
            break;
        }
        total_outliers = n_flagged;
        NF_Point2 *pts_in  = (NF_Point2*)xmalloc(n_inliers * sizeof(NF_Point2));
        double    *t_in    = (double*)   xmalloc(n_inliers * sizeof(double));
        int k = 0;
        for (int i = 0; i < n_pts; i++) {
            if (inlier_mask[i]) {
                pts_in[k] = ordered[i];
                t_in[k]   = t_params[i];
                k++;
            }
        }
        free(c->knots);
        c->knots = make_knots(t_in, n_inliers, n, p);
        fit_pass(pts_in, n_inliers, t_in, NULL, c->knots, n, p, &cfg, c, &cond);
        result.condition_number = cond;
        free(pts_in); free(t_in);
    }
    if (total_outliers > 0)
        result.quality |= NF_FIT_OUTLIERS;
    if (cfg.local_outlier_iters > 0) {
        for (int lo_pass = 0;
             lo_pass < cfg.local_outlier_iters;
             lo_pass++) {
            int n_local = local_outlier_rejection(
                c, ordered, n_pts, t_params,
                inlier_mask,
                cfg.local_outlier_sigma,
                cfg.local_outlier_bands);
            if (n_local == 0) break;   
            total_outliers += n_local;
            int n_in = 0;
            for (int i = 0; i < n_pts; i++)
                if (inlier_mask[i] && !cv_mask[i]) n_in++;
            if (n_in < n + 2) break;   
            NF_Point2 *pts_in = (NF_Point2*)xmalloc(n_in*sizeof(NF_Point2));
            double    *t_in   = (double*)   xmalloc(n_in*sizeof(double));
            int k = 0;
            for (int i = 0; i < n_pts; i++) {
                if (inlier_mask[i] && !cv_mask[i]) {
                    pts_in[k] = ordered[i];
                    t_in[k]   = t_params[i];
                    k++;
                }
            }
            free(c->knots);
            c->knots = make_knots(t_in, n_in, n, p);
            fit_pass(pts_in, n_in, t_in, NULL,
                     c->knots, n, p, &cfg, c, &cond);
            result.condition_number = cond;
            free(pts_in); free(t_in);
        }
    }
    result.n_outliers = total_outliers;
    NF_Point2 *pts_work  = (NF_Point2*)xmalloc(n_pts * sizeof(NF_Point2));
    double    *t_work    = (double*)   xmalloc(n_pts * sizeof(double));
    int        m_work    = 0;
    for (int i = 0; i < n_pts; i++) {
        if (inlier_mask[i]) {
            pts_work[m_work] = ordered[i];
            t_work[m_work]   = t_params[i];
            m_work++;
        }
    }
    result.fold_detected = 0;
    result.fold_x_end    = 0.0;
    if (cfg.fold_detect) {
        double fold_x = detect_fold_x_end(c);
        if (fold_x > -0.5) {    
            result.fold_detected = 1;
            result.fold_x_end    = fold_x;
            double x_data_span_9a = ordered[n_pts-1].x - ordered[0].x;
            double x_cut;
            if (fold_x >= 0.0) {
                double margin = fold_x * 0.10;
                if (margin < 0.002 * x_data_span_9a) margin = 0.002 * x_data_span_9a;
                x_cut = fold_x + margin;
            } else {
                double margin = (-fold_x) * 2.0;
                if (margin < 0.005 * x_data_span_9a) margin = 0.005 * x_data_span_9a;
                x_cut = margin;
            }
            int m_new = 0;
            for (int i = 0; i < n_pts; i++) {
                if (inlier_mask[i] && !cv_mask[i] &&
                    ordered[i].x >= x_cut)
                    m_new++;
            }
            if (m_new >= n + 2) {   
                NF_Point2 *pts_fold = (NF_Point2*)xmalloc(m_new*sizeof(NF_Point2));
                double    *t_fold   = (double*)xmalloc(m_new*sizeof(double));
                int k2 = 0;
                for (int i = 0; i < n_pts; i++) {
                    if (inlier_mask[i] && !cv_mask[i] &&
                        ordered[i].x >= x_cut) {
                        pts_fold[k2] = ordered[i];
                        t_fold[k2]   = t_params[i];
                        k2++;
                    }
                }
                double *t_new = centripetal_parameterise(pts_fold, m_new);
                free(c->knots);
                c->knots = make_knots(t_new, m_new, n, p);
                fit_pass(pts_fold, m_new, t_new, NULL,
                         c->knots, n, p, &cfg, c, &cond);
                result.condition_number = cond;
                free(pts_work); free(t_work);
                pts_work = pts_fold;
                t_work   = t_new;
                m_work   = m_new;
                free(t_fold);
            }
        }
    }
    int adapted = 0;
    for (int adapt_iter = 0;
         adapt_iter < cfg.adaptive_iters;
         adapt_iter++) {
        int     n_segs    = c->n_ctrl - p;    
        double *seg_rms   = (double*)xmalloc(n_segs * sizeof(double));
        int    *seg_count = (int*)   xmalloc(n_segs * sizeof(int));
        compute_segment_rms(c, pts_work, m_work, t_work,
                            seg_rms, seg_count);
        double global_rms = 0.0;
        for (int k = 0; k < n_segs; k++)
            global_rms += seg_rms[k] * seg_rms[k] * seg_count[k];
        global_rms = (m_work > 0) ? sqrt(global_rms / m_work) : 0.0;
        double *new_knots_to_insert = (double*)xmalloc(n_segs * sizeof(double));
        int     n_to_insert         = 0;
        for (int k = 0; k < n_segs; k++) {
            if (seg_rms[k] > cfg.adaptive_threshold * global_rms
                && seg_count[k] > 0) {
                double t_new = 0.5 * (c->knots[p+k] + c->knots[p+k+1]);
                if (t_new > c->knots[p+k] + 1e-10
                    && t_new < c->knots[p+k+1] - 1e-10) {
                    new_knots_to_insert[n_to_insert++] = t_new;
                }
            }
        }
        free(seg_rms); free(seg_count);
        if (n_to_insert == 0) break;    
        int n_new = c->n_ctrl + n_to_insert;
        if (n_new > cfg.n_ctrl_max) {
            result.quality |= NF_FIT_BAD_NOCONV;
            free(new_knots_to_insert);
            break;
        }
        double *U_new = make_knots(t_work, m_work, n_new, p);
        free(c->knots);
        free(c->ctrl_wx);
        free(c->ctrl_wy);
        free(c->weights);
        c->n_ctrl  = n_new;
        c->knots   = U_new;
        c->ctrl_wx = (double*)xmalloc(n_new * sizeof(double));
        c->ctrl_wy = (double*)xmalloc(n_new * sizeof(double));
        c->weights = (double*)xmalloc(n_new * sizeof(double));
        n = n_new;
        free(new_knots_to_insert);
        fit_pass(pts_work, m_work, t_work, NULL, c->knots, n, p, &cfg, c, &cond);
        result.condition_number = cond;
        adapted = 1;
        if (cfg.reparam_iters > 0) {
            for (int rp = 0; rp < cfg.reparam_iters; rp++) {
                int improved = reparameterise_inplace(c, pts_work, m_work, t_work);
                if (!improved) {
                    result.quality |= NF_FIT_REPARAM_SKIP;
                    break;
                }
                free(c->knots);
                c->knots = make_knots(t_work, m_work, n, p);
                fit_pass(pts_work, m_work, t_work, NULL, c->knots,
                         n, p, &cfg, c, &cond);
                result.condition_number = cond;
            }
        }
        if (n_cv > 0) {
            result.cv_score = compute_cv_score(c, ordered, n_pts,
                                               t_params, cv_mask,
                                               cfg.outlier_sigma);
            double fit_sse = 0.0;
            for (int i = 0; i < m_work; i++) {
                NF_Point2 ev = nf_eval(c, t_work[i]);
                fit_sse += sq(ev.x-pts_work[i].x) + sq(ev.y-pts_work[i].y);
            }
            double fit_rms = (m_work > 0) ? sqrt(fit_sse/m_work) : 0.0;
            if (fit_rms > 0 &&
                result.cv_score > cfg.cv_overfit_ratio * fit_rms) {
                result.quality |= NF_FIT_CV_MARGINAL;
                break;    
            }
        }
        if (cfg.y_min < cfg.y_max) {
            double y_range    = cfg.y_max - cfg.y_min;
            double margin_pct = 0.02 * y_range;
            double margin_rms = 10.0 * result.rms;
            double margin     = (margin_pct > margin_rms) ? margin_pct : margin_rms;
            double lo         = cfg.y_min - margin;
            double hi         = cfg.y_max + margin;
            int    oob        = 0;
            for (int i = 0; i <= 500 && !oob; i++) {
                double    t  = (double)i / 500;
                NF_Point2 pt = nf_eval(c, t);
                if (pt.y < lo || pt.y > hi) oob = 1;
            }
            if (oob) break;    
        }
    }
    if (adapted) result.quality |= NF_FIT_ADAPTED;
    if (n_cv > 0)
        result.cv_score = compute_cv_score(c, ordered, n_pts,
                                           t_params, cv_mask,
                                           cfg.outlier_sigma);
    result.n_ctrl_final = c->n_ctrl;
    if (n_cv > 0 && result.rms > 0.0) {
        double cv_ratio = result.cv_score / result.rms;
        if (cv_ratio > cfg.cv_overfit_ratio)
            result.quality |= NF_FIT_CV_MARGINAL;
        if (cfg.cv_fatal_ratio > 0.0 && cv_ratio > cfg.cv_fatal_ratio)
            result.quality |= NF_FIT_BAD_OVERFIT;
    }
    if (cfg.irls_iters > 0) {
        double *w_data = (double*)xmalloc(m_work * sizeof(double));
        for (int iter = 0; iter < cfg.irls_iters; iter++) {
             
            for (int i = 0; i < m_work; i++) {
                NF_Point2 ev  = nf_eval(c, t_work[i]);
                double    res = sqrt(sq(ev.x-pts_work[i].x) +
                                     sq(ev.y-pts_work[i].y));
                w_data[i] = 1.0 / (res + cfg.irls_epsilon);
            }
            fit_pass(pts_work, m_work, t_work, w_data, c->knots,
                     n, p, &cfg, c, NULL);
        }
        free(w_data);
    }
    if (result.condition_number > 1e8)
        result.quality |= NF_FIT_BAD_CONDNUM;
    {
        double *xs_sorted = (double*)xmalloc(m_work * sizeof(double));
        for (int i = 0; i < m_work; i++) xs_sorted[i] = pts_work[i].x;
        for (int i = 1; i < m_work; i++) {
            double key = xs_sorted[i]; int j = i-1;
            while (j >= 0 && xs_sorted[j] > key) {
                xs_sorted[j+1] = xs_sorted[j]; j--;
            }
            xs_sorted[j+1] = key;
        }
        double x_range = xs_sorted[m_work-1] - xs_sorted[0];
        double max_gap = 0.0;
        for (int i = 1; i < m_work; i++) {
            double gap = xs_sorted[i] - xs_sorted[i-1];
            if (gap > max_gap) max_gap = gap;
        }
        free(xs_sorted);
        double gap_fraction = (x_range > 1e-10) ? max_gap / x_range : 0.0;
        if (gap_fraction > 0.15) {
            result.quality |= NF_FIT_BAD_CONDNUM;
            if (result.condition_number < 1e9)
                result.condition_number = 1e9 * gap_fraction;
        }
    }
    {
        double sse = 0.0;
        for (int i = 0; i < m_work; i++) {
            NF_Point2 ev = nf_eval(c, t_work[i]);
            sse += sq(ev.x-pts_work[i].x) + sq(ev.y-pts_work[i].y);
        }
        result.rms = (m_work > 0) ? sqrt(sse/m_work) : 0.0;
        if (total_outliers > 0) {
            double sse_out = 0.0;
            int    n_out   = 0;
            for (int i = 0; i < n_pts; i++) {
                if (inlier_mask[i] || cv_mask[i]) continue;
                NF_Point2 ev = nf_eval(c, t_params[i]);
                sse_out += sq(ev.x-ordered[i].x) + sq(ev.y-ordered[i].y);
                n_out++;
            }
            result.rms_outlier = (n_out > 0) ? sqrt(sse_out/n_out) : 0.0;
        }
    }
    if (cfg.y_min < cfg.y_max) {
        double y_range    = cfg.y_max - cfg.y_min;
        double margin_pct = 0.02 * y_range;
        double margin_rms = 10.0 * result.rms;
        double margin     = (margin_pct > margin_rms) ? margin_pct : margin_rms;
        double lo         = cfg.y_min - margin;
        double hi         = cfg.y_max + margin;
        int n_scan        = 1000;
        int out_of_bounds = 0;
        for (int i = 0; i <= n_scan; i++) {
            double    t  = (double)i / n_scan;
            NF_Point2 pt = nf_eval(c, t);
            if (pt.y < lo || pt.y > hi) {
                out_of_bounds = 1;
                break;
            }
        }
        if (out_of_bounds)
            result.quality |= NF_FIT_BAD_BOUNDS;
    }
    if (cfg.fold_detect) {
        int final_fold_passes = 0;
        int max_final_passes  = 5;
        int n_refit = n;    
        while (final_fold_passes < max_final_passes) {
            double fold_x = detect_fold_x_end(c);
            if (fold_x <= -0.5) break;    
            result.fold_detected = 1;
            result.fold_x_end    = fold_x;
            final_fold_passes++;
            double x_cut;
            double x_data_span = ordered[n_pts-1].x - ordered[0].x;
            if (fold_x >= 0.0) {
                double margin = fold_x * (0.20 + 0.10 * final_fold_passes);
                double min_margin = 0.003 * x_data_span;
                if (margin < min_margin) margin = min_margin;
                x_cut = fold_x + margin;
            } else {
                double margin = (-fold_x) * 2.0;
                 
                double min_margin2 = 0.005 * x_data_span;
                if (margin < min_margin2) margin = min_margin2;
                x_cut = margin;
            }
            double fold_relative = (x_data_span > 0) ? fold_x / x_data_span : 0.0;
            if (fold_relative > 0.05 && cond > 200 && n_refit > p + 2) {
                n_refit -= 2;
                if (n_refit < p + 2) n_refit = p + 2;
            }
            int m_new = 0;
            for (int i = 0; i < n_pts; i++)
                if (inlier_mask[i] && !cv_mask[i] && ordered[i].x >= x_cut)
                    m_new++;
            if (m_new < n_refit + 2) break;    
            NF_Point2 *pts2 = (NF_Point2*)xmalloc(m_new*sizeof(NF_Point2));
            double    *t2   = (double*)   xmalloc(m_new*sizeof(double));
            int k2 = 0;
            for (int i = 0; i < n_pts; i++)
                if (inlier_mask[i] && !cv_mask[i] && ordered[i].x >= x_cut) {
                    pts2[k2] = ordered[i];
                    t2[k2]   = t_params[i];
                    k2++;
                }
            double *t_new2 = centripetal_parameterise(pts2, m_new);
            free(c->knots);
            c->knots = make_knots(t_new2, m_new, n_refit, p);
            fit_pass(pts2, m_new, t_new2, NULL,
                     c->knots, n_refit, p, &cfg, c, &cond);
            result.condition_number = cond;
            result.n_ctrl_final     = n_refit;
            free(pts2); free(t2); free(t_new2);
        }
    }
    free(ordered);
    free(t_params);
    free(inlier_mask);
    free(cv_mask);
    free(pts_work);
    free(t_work);
    free(pts_filtered);   
    if (result_out) *result_out = result;
    return c;
}

// This function not currently used.
double nf_compute_rms(const NF_Curve *c,
                      const NF_Point2 *pts, int n_pts,
                      const double *t_params)
{
    double sse = 0.0;
    for (int i = 0; i < n_pts; i++) {
        double t;
        if (t_params) {
            t = t_params[i];
        } else {
             
            double best_d = DBL_MAX;
            t = 0.0;
            int scan_steps = 200;
            for (int k = 0; k <= scan_steps; k++) {
                double tk = (double)k / scan_steps;
                NF_Point2 ev = nf_eval(c, tk);
                double d = sq(ev.x-pts[i].x) + sq(ev.y-pts[i].y);
                if (d < best_d) { best_d = d; t = tk; }
            }
            for (int iter = 0; iter < 8; iter++) {
                NF_Point2 ev = nf_eval(c, t);
                NF_Point2 dv = nf_deriv(c, t);
                double f  = (ev.x-pts[i].x)*dv.x + (ev.y-pts[i].y)*dv.y;
                double df = dv.x*dv.x + dv.y*dv.y;
                if (df < 1e-20) break;
                double step = f/df;
                if (step > 0.05) step=0.05;
                if (step <-0.05) step=-0.05;
                t -= step;
                if (t < 0) t=0; if (t > 1) t=1;
                if (fabs(step) < 1e-10) break;
            }
        }
        NF_Point2 ev = nf_eval(c, t);
        sse += sq(ev.x-pts[i].x) + sq(ev.y-pts[i].y);
    }
    return sqrt(sse / n_pts);
}

// This function not currently used.
NF_Point2 *nf_sample(const NF_Curve *c, int n_samples)
{
    NF_Point2 *out = (NF_Point2*)xmalloc(n_samples * sizeof(NF_Point2));
    for (int i = 0; i < n_samples; i++) {
        double t = (double)i / (double)(n_samples-1);
        out[i] = nf_eval(c, t);
    }
    return out;
}

void nf_curve_free(NF_Curve *c)
{
    if (!c) return;
    free(c->knots); free(c->ctrl_wx);
    free(c->ctrl_wy); free(c->weights);
    free(c);
}

// This function not currently used.
int nf_curve_write(const NF_Curve *c, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "degree %d\nn_ctrl %d\nknots", c->degree, c->n_ctrl);
    int nk = c->n_ctrl + c->degree + 1;
    for (int i = 0; i < nk; i++) fprintf(f, " %.17g", c->knots[i]);
    fprintf(f, "\nctrl_points\n");
    for (int i = 0; i < c->n_ctrl; i++)
        fprintf(f, "  %.17g %.17g %.17g\n",
                c->ctrl_wx[i], c->ctrl_wy[i], c->weights[i]);
    fclose(f);
    return 0;
}

// This function not currently used.
NF_Curve *nf_curve_read(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    NF_Curve *c = (NF_Curve*)xmalloc(sizeof(NF_Curve));
    memset(c, 0, sizeof(*c));
    char key[64];
    if (fscanf(f, "%63s %d", key, &c->degree)  != 2) goto err;
    if (fscanf(f, "%63s %d", key, &c->n_ctrl)  != 2) goto err;
    int nk = c->n_ctrl + c->degree + 1;
    c->knots   = (double*)xmalloc(nk * sizeof(double));
    c->ctrl_wx = (double*)xmalloc(c->n_ctrl * sizeof(double));
    c->ctrl_wy = (double*)xmalloc(c->n_ctrl * sizeof(double));
    c->weights = (double*)xmalloc(c->n_ctrl * sizeof(double));
    if (fscanf(f, "%63s", key) != 1) goto err;
    for (int i = 0; i < nk; i++)
        if (fscanf(f, "%lf", &c->knots[i]) != 1) goto err;
    if (fscanf(f, "%63s", key) != 1) goto err;
    for (int i = 0; i < c->n_ctrl; i++)
        if (fscanf(f, "%lf %lf %lf",
                   &c->ctrl_wx[i], &c->ctrl_wy[i], &c->weights[i]) != 3)
            goto err;
    fclose(f);
    return c;
err:
    fclose(f);
    nf_curve_free(c);
    return NULL;
}

// This function not currently used.
static double *make_uniform_knots_x(double x_lo, double x_hi,
                                     int n_ctrl, int p)
{
    int n_knots = n_ctrl + p + 1;
    double *U   = (double*)xmalloc(n_knots * sizeof(double));
    int n_interior = n_ctrl - p - 1;
    for (int i = 0; i <= p; i++) U[i] = x_lo;
    for (int i = 1; i <= n_interior; i++)
        U[p + i] = x_lo + (x_hi - x_lo) * i / (n_interior + 1);
    for (int i = 0; i <= p; i++) U[n_knots - 1 - i] = x_hi;
    return U;
}

// This function not currently used.
static int outlier_mask_direct(const NF_Curve   *c,
                                const NF_Point2  *pts,
                                int               m,
                                double            sigma_thresh,
                                char             *inlier_mask)
{
    double *res = (double*)xmalloc(m * sizeof(double));
    double  mean_res = 0.0;
    for (int i = 0; i < m; i++) {
         
        NF_Point2 ev = nf_eval(c, pts[i].x);
        res[i]    = fabs(ev.y - pts[i].y);
        mean_res += res[i];
    }
    mean_res /= m;
    double var = 0.0;
    for (int i = 0; i < m; i++) var += sq(res[i] - mean_res);
    double sigma = sqrt(var / m);
    double thr   = sigma_thresh * sigma;
    int n_outliers = 0;
    for (int i = 0; i < m; i++) {
        if (res[i] > thr) { inlier_mask[i] = 0; n_outliers++; }
        else              { inlier_mask[i] = 1; }
    }
    free(res);
    return n_outliers;
}

// This function not currently used - tried this, see notes.
NF_Curve *nf_fit_direct(const NF_Point2 *pts,
                         int              n_pts,
                         const NF_Config *cfg_in,
                         NF_FitResult    *result_out)
{
    NF_Config cfg;
    if (cfg_in) cfg = *cfg_in;
    else        nf_default_config(&cfg);
    int p = cfg.degree;
    if (p < 1) p = 1;
    if (p > 9) p = 9;
    int n_segs = cfg.direct_n_segments > 0 ? cfg.direct_n_segments : 22;
    int n_ctrl = n_segs + p;    
    NF_FitResult result;
    memset(&result, 0, sizeof(result));
    result.quality          = NF_FIT_OK | NF_FIT_DIRECT;
    result.condition_number = 1.0;
    result.n_ctrl_initial   = n_ctrl;
    result.n_ctrl_final     = n_ctrl;
    result.ordering_used    = NF_ORDER_BY_X;   
    if (n_pts < p + 2) {
        result.quality |= NF_FIT_BAD_TOOFEW;
        if (result_out) *result_out = result;
        return NULL;
    }
    NF_Point2 *pts_filtered = NULL;
    int need_filter = (cfg.pre_filter_x_min > 0.0 || cfg.pre_filter_y_max > 0.0);
    if (need_filter) {
        int n_keep = 0;
        for (int i = 0; i < n_pts; i++) {
            if (cfg.pre_filter_x_min > 0.0 && pts[i].x < cfg.pre_filter_x_min) continue;
            if (cfg.pre_filter_y_max > 0.0 && pts[i].y > cfg.pre_filter_y_max) continue;
            n_keep++;
        }
        if (n_keep < n_pts) {
            pts_filtered = (NF_Point2*)xmalloc(n_keep * sizeof(NF_Point2));
            int j = 0;
            for (int i = 0; i < n_pts; i++) {
                if (cfg.pre_filter_x_min > 0.0 && pts[i].x < cfg.pre_filter_x_min) continue;
                if (cfg.pre_filter_y_max > 0.0 && pts[i].y > cfg.pre_filter_y_max) continue;
                pts_filtered[j++] = pts[i];
            }
            pts   = pts_filtered;
            n_pts = n_keep;
            result.quality |= NF_FIT_PRE_FILTERED;
        }
    }
    if (n_pts < n_ctrl + 1) {
        result.quality |= NF_FIT_BAD_TOOFEW;
        free(pts_filtered);
        if (result_out) *result_out = result;
        return NULL;
    }
    NF_Point2 *sorted = (NF_Point2*)xmalloc(n_pts * sizeof(NF_Point2));
    memcpy(sorted, pts, n_pts * sizeof(NF_Point2));
    qsort(sorted, n_pts, sizeof(NF_Point2), cmp_by_x);
    double x_lo = sorted[0].x;
    double x_hi = sorted[n_pts-1].x;
    if (cfg.pin_end && cfg.end_pt.x > x_hi) x_hi = cfg.end_pt.x;
    if (cfg.pin_start && cfg.start_pt.x < x_lo) x_lo = cfg.start_pt.x;
    double *t_params = (double*)xmalloc(n_pts * sizeof(double));
    for (int i = 0; i < n_pts; i++) t_params[i] = sorted[i].x;
    double *U = make_uniform_knots_x(x_lo, x_hi, n_ctrl, p);
    NF_Curve *c = (NF_Curve*)xcalloc(1, sizeof(NF_Curve));
    c->degree   = p;
    c->n_ctrl   = n_ctrl;
    c->knots    = U;
    c->ctrl_wx  = (double*)xmalloc(n_ctrl * sizeof(double));
    c->ctrl_wy  = (double*)xmalloc(n_ctrl * sizeof(double));
    c->weights  = (double*)xmalloc(n_ctrl * sizeof(double));
    for (int i = 0; i < n_ctrl; i++) c->weights[i] = 1.0;
    double cond = 1.0;
    fit_pass(sorted, n_pts, t_params, NULL,
             U, n_ctrl, p, &cfg, c, &cond);
    result.condition_number = cond;
    char   *inlier_mask = (char*)xmalloc(n_pts);
    memset(inlier_mask, 1, n_pts);
    int total_outliers = 0;
    for (int iter = 0; iter < cfg.outlier_iters; iter++) {
        int n_out = outlier_mask_direct(c, sorted, n_pts,
                                        cfg.outlier_sigma, inlier_mask);
        if (n_out == 0) break;
        total_outliers = 0;
        for (int i = 0; i < n_pts; i++) if (!inlier_mask[i]) total_outliers++;
        int n_inliers = n_pts - total_outliers;
        if (n_inliers < n_ctrl + 1) break;    
        NF_Point2 *pts_in  = (NF_Point2*)xmalloc(n_inliers * sizeof(NF_Point2));
        double    *t_in    = (double*)   xmalloc(n_inliers * sizeof(double));
        int k = 0;
        for (int i = 0; i < n_pts; i++) {
            if (inlier_mask[i]) {
                pts_in[k] = sorted[i];
                t_in[k]   = t_params[i];
                k++;
            }
        }
        fit_pass(pts_in, n_inliers, t_in, NULL,
                 U, n_ctrl, p, &cfg, c, &cond);
        result.condition_number = cond;
        free(pts_in); free(t_in);
    }
    if (total_outliers > 0) {
        result.quality    |= NF_FIT_OUTLIERS;
        result.n_outliers  = total_outliers;
    }
    {
        double sum2 = 0.0; int n_in = 0;
        for (int i = 0; i < n_pts; i++) {
            if (!inlier_mask[i]) continue;
            NF_Point2 ev = nf_eval(c, sorted[i].x);
            sum2 += sq(ev.y - sorted[i].y);
            n_in++;
        }
        result.rms = n_in > 0 ? sqrt(sum2 / n_in) : 0.0;
    }
    if (cond > 1e8) result.quality |= NF_FIT_BAD_CONDNUM;
    free(sorted);
    free(t_params);
    free(inlier_mask);
    free(pts_filtered);
    result.spearman_rho = -2.0;    
    if (result_out) *result_out = result;
    return c;
}

