/*  nurbs_spline.c

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

 /* Clean-room implementation of not-a-knot cubic spline construction
  * derived from standard published numerical methods (Thomas algorithm
  * for tridiagonal systems; not-a-knot end conditions as described in
  * de Boor, C., "A Practical Guide to Splines", Springer, 1978).
  * No GPL-encumbered or proprietary source code was consulted
  * in the writing of this file.
  */

#include "nurbs_spline.h"
#include "nurbs_fit.h"

#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef NAN
#  define NAN (0.0/0.0)
#endif

#define NS_EDGE_CLAMP_TOL 1e-4

#ifndef NS_EXTEND_PTS
#define NS_EXTEND_PTS 16
#endif

static void *ns_malloc(size_t n)
{
    void *p = malloc(n);
    if (!p) { fprintf(stderr,"nurbs_spline: out of memory\n"); exit(1); }
    return p;
}
static void *ns_calloc(size_t n, size_t sz)
{
    void *p = calloc(n,sz);
    if (!p) { fprintf(stderr,"nurbs_spline: out of memory\n"); exit(1); }
    return p;
}
static double ns_sq(double x) { return x*x; }

static void build_spline_segments(const double *xs, const double *ys,
                                   int n, NS_Seg *segs)
{
    int m = n - 1;

    if (m < 1) return;
    if (m == 1) {
        segs[0].x0 = xs[0];
        segs[0].a  = ys[0];
        segs[0].b  = (xs[1] > xs[0]) ? (ys[1] - ys[0]) / (xs[1] - xs[0]) : 0.0;
        segs[0].c  = 0.0;
        segs[0].d  = 0.0;
        return;
    }

    double *h = (double*)ns_malloc(m * sizeof(double));
    for (int i = 0; i < m; i++)
        h[i] = xs[i+1] - xs[i];

    int ni = n - 2;   /* number of interior unknowns */

    if (ni < 1) {
        free(h);
        return;
    }

    double *sub = (double*)ns_malloc(ni * sizeof(double));
    double *dia = (double*)ns_malloc(ni * sizeof(double));
    double *sup = (double*)ns_malloc(ni * sizeof(double));
    double *rhs = (double*)ns_malloc(ni * sizeof(double));

    for (int k = 0; k < ni; k++) {
        int i   = k + 1;
        sub[k]  = h[i-1];
        dia[k]  = 2.0 * (h[i-1] + h[i]);
        sup[k]  = h[i];
        rhs[k]  = 6.0 * ((ys[i+1] - ys[i]) / h[i]
                         -(ys[i]   - ys[i-1]) / h[i-1]);
    }
    {
        double f0 = h[0] / h[1];
        dia[0] += f0 * (h[0] + h[1]);
        if (ni > 1) sup[0] -= f0 * h[0];
        sub[0]  = 0.0;
    }
    {
        double fN = h[m-1] / h[m-2];
        dia[ni-1] += fN * (h[m-2] + h[m-1]);
        if (ni > 1) sub[ni-1] -= fN * h[m-1];
        sup[ni-1]  = 0.0;
    }
    for (int k = 1; k < ni; k++) {
        if (fabs(dia[k-1]) < 1e-15) continue;
        double factor = sub[k] / dia[k-1];
        dia[k] -= factor * sup[k-1];
        rhs[k] -= factor * rhs[k-1];
    }
    double *M = (double*)ns_calloc(n, sizeof(double));
    M[ni] = (fabs(dia[ni-1]) > 1e-15) ? rhs[ni-1] / dia[ni-1] : 0.0;
    for (int k = ni-2; k >= 0; k--) {
        double r = rhs[k] - sup[k] * M[k+2];
        M[k+1] = (fabs(dia[k]) > 1e-15) ? r / dia[k] : 0.0;
    }

    M[0]   = ((h[0] + h[1]) * M[1]   - h[0]   * M[2])   / h[1];
    M[n-1] = ((h[m-2]+h[m-1])* M[n-2] - h[m-1] * M[n-3]) / h[m-2];

    for (int i = 0; i < m; i++) {
        segs[i].x0 = xs[i];
        segs[i].a  = ys[i];
        segs[i].b  = (ys[i+1] - ys[i]) / h[i] - h[i] * (2.0*M[i] + M[i+1]) / 6.0;
        segs[i].c  = M[i] / 2.0;
        segs[i].d  = (M[i+1] - M[i]) / (6.0 * h[i]);
    }

    free(h); free(sub); free(dia); free(sup); free(rhs); free(M);
}

#define NS_MAX_BRANCHES 16

static int find_branches(const double *xs_t, int n,
                          int *branch_start, int *branch_end)
{
    int nb = 0;
    branch_start[0] = 0;
    int going_up = 1;
    for (int i = 1; i < n; i++) {
        if (xs_t[i] != xs_t[i-1]) {
            going_up = (xs_t[i] > xs_t[i-1]);
            break;
        }
    }

    for (int i = 1; i < n; i++) {
        if (xs_t[i] == xs_t[i-1]) continue;
        int now_up = (xs_t[i] > xs_t[i-1]);
        if (now_up != going_up) {
            branch_end[nb] = i - 1;
            nb++;

            if (nb >= NS_MAX_BRANCHES - 1) {
                branch_end[nb - 1] = n - 1;
                return nb;
            }
            branch_start[nb] = i - 1;
            going_up = now_up;
        }
    }
    branch_end[nb] = n - 1;
    nb++;
    return nb;
}

NS_Spline *ns_build(const NF_Curve    *curve,
                    const NF_FitResult *result,
                    int                 n_pts)
{
    if (!curve) return NULL;

    if (result && (result->quality & NF_FIT_BAD)) {
        fprintf(stderr,
            "ns_build: refusing to build spline from a bad fit "
            "(quality=0x%03x)\n", result->quality);
        return NULL;
    }

    if (n_pts <= 0) {
        int n_by_ctrl = NS_SAMPLES_PER_CTRL * curve->n_ctrl;
        n_pts = (n_by_ctrl > NS_DEFAULT_PTS) ? n_by_ctrl : NS_DEFAULT_PTS;
    }
    if (n_pts < 4) n_pts = 4;

    double *xs_t = (double*)ns_malloc(n_pts * sizeof(double));
    double *ys_t = (double*)ns_malloc(n_pts * sizeof(double));
    double *ts   = (double*)ns_malloc(n_pts * sizeof(double));

    for (int i = 0; i < n_pts; i++) {
        double t  = (double)i / (double)(n_pts-1);
        NF_Point2 pt = nf_eval(curve, t);
        xs_t[i] = pt.x;
        ys_t[i] = pt.y;
        ts[i]   = t;
    }

    int branch_start[NS_MAX_BRANCHES];
    int branch_end  [NS_MAX_BRANCHES];
    int nb = find_branches(xs_t, n_pts, branch_start, branch_end);

    NS_Spline *s = (NS_Spline*)ns_calloc(1, sizeof(NS_Spline));
    s->n_branches = nb;
    s->branches   = (NS_Branch*)ns_calloc(nb, sizeof(NS_Branch));

    for (int b = 0; b < nb; b++) {
        int i0 = branch_start[b];
        int i1 = branch_end  [b];
        int np = i1 - i0 + 1;

        NS_Branch *br = &s->branches[b];
        br->n_pts  = np;
        br->xs     = (double*)ns_malloc(np * sizeof(double));
        br->ys     = (double*)ns_malloc(np * sizeof(double));
        br->segs   = (NS_Seg *)ns_malloc((np-1) * sizeof(NS_Seg));

        int ascending = (xs_t[i1] >= xs_t[i0]);
        for (int i = 0; i < np; i++) {
            int src = ascending ? (i0+i) : (i1-i);
            br->xs[i] = xs_t[src];
            br->ys[i] = ys_t[src];
        }

        double x_merge_tol = 1e-9;
        int nclean = 1;
        for (int i = 1; i < np; i++) {
            if (br->xs[i] > br->xs[nclean-1] + x_merge_tol) {
                br->xs[nclean] = br->xs[i];
                br->ys[nclean] = br->ys[i];
                nclean++;
            }
        }
        br->n_pts = nclean;

        if (nclean < 4) {
            br->n_pts = 0;
            br->t_mid = 0.5 * (ts[ascending ? i0 : i1] +
                               ts[ascending ? i1 : i0]);
            continue;
        }
        br->t_mid = 0.5 * (ts[ascending ? i0 : i1] +
                           ts[ascending ? i1 : i0]);

        build_spline_segments(br->xs, br->ys, nclean, br->segs);

        double branch_x_span = (nclean > 1) ? br->xs[nclean-1] - br->xs[0] : 1.0;
        double branch_y_lo   = br->ys[0], branch_y_hi = br->ys[0];
        for (int i = 1; i < nclean; i++) {
            if (br->ys[i] < branch_y_lo) branch_y_lo = br->ys[i];
            if (br->ys[i] > branch_y_hi) branch_y_hi = br->ys[i];
        }
        double branch_y_span = branch_y_hi - branch_y_lo;
        if (branch_y_span < 1e-6) branch_y_span = 0.01 * branch_x_span;
        if (branch_y_span < 1e-9) branch_y_span = 1e-9;

        for (int i = 0; i < nclean - 1; i++) {
            NS_Seg *seg    = &br->segs[i];
            double  dx_seg = br->xs[i+1] - br->xs[i];
            double  dy_pred = seg->b * dx_seg
                            + seg->c * dx_seg * dx_seg
                            + seg->d * dx_seg * dx_seg * dx_seg;
            int degenerate =
                (fabs(seg->b) > 1e6) ||
                (fabs(seg->c) > 1e6) ||
                (fabs(seg->d) > 1e6) ||
                (fabs(dy_pred) > 2.0 * branch_y_span + 1.0) ||
                (dx_seg < 1e-4 * branch_x_span &&
                 fabs(dy_pred) > 0.05 * branch_y_span);

            if (degenerate) {
                double dy = br->ys[i+1] - br->ys[i];
                seg->a = br->ys[i];
                seg->b = (dx_seg > 1e-15) ? dy / dx_seg : 0.0;
                seg->c = 0.0;
                seg->d = 0.0;
            }
        }
    }

    free(xs_t); free(ys_t); free(ts);
    return s;
}

int ns_extend_left(NS_Spline *s,
                   double x_target,
                   double x_anchor,
                   double bound_frac,
                   double y_lo_clamp,
                   double y_hi_clamp)
{
    if (!s || s->n_branches < 1) return 0;

    int    best_b   = -1;
    double best_xlo = 0.0;
    for (int b = 0; b < s->n_branches; b++) {
        NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        if (best_b < 0 || br->xs[0] < best_xlo) {
            best_b   = b;
            best_xlo = br->xs[0];
        }
    }
    if (best_b < 0) return 0;

    NS_Branch *br = &s->branches[best_b];
    double x0 = br->xs[0];

    if (x0 <= x_target + 1e-9) return 0;

    double xb = x_anchor;
    if (xb <= x0)              xb = x0;
    double x_right = br->xs[br->n_pts - 1];
    if (xb >= x_right)         xb = x0;

    int seg_i;
    if (xb <= br->xs[0])              seg_i = 0;
    else if (xb >= br->xs[br->n_pts-1]) seg_i = br->n_pts - 2;
    else {
        int lo = 0, hi = br->n_pts - 1;
        while (hi - lo > 1) {
            int mid = (lo + hi) >> 1;
            if (xb < br->xs[mid]) hi = mid; else lo = mid;
        }
        seg_i = lo;
    }
    double dxb  = xb - br->segs[seg_i].x0;
    double A    = br->segs[seg_i].a;
    double B    = br->segs[seg_i].b;
    double C    = br->segs[seg_i].c;
    double D    = br->segs[seg_i].d;
    double yb   = A + dxb * (B + dxb * (C + dxb * D));
    double ypb  = B + dxb * (2.0 * C + dxb * 3.0 * D);
    double yppb = 2.0 * C + dxb * 6.0 * D;
    double L = xb - x_target;
    double A3 = yppb / (6.0 * L);
    double A1 = ypb - L * yppb / 2.0;
    double A0 = yb - L * ypb + L * L * yppb / 3.0;
    double y_natural = A0;
    double bf = (bound_frac > 0.0) ? bound_frac : 0.0;
    double lo_bnd = yb - bf * fabs(yb);
    double hi_bnd = yb + bf * fabs(yb);
    if (lo_bnd < y_lo_clamp) lo_bnd = y_lo_clamp;
    if (hi_bnd > y_hi_clamp) hi_bnd = y_hi_clamp;

    int    use_quintic = 0;
    double y_targ      = y_natural;
    if (y_targ < lo_bnd) { y_targ = lo_bnd; use_quintic = 1; }
    if (y_targ > hi_bnd) { y_targ = hi_bnd; use_quintic = 1; }
    double q_a0=0, q_a3=0, q_a4=0, q_a5=0;
    if (use_quintic) {
        q_a0 = y_targ;
        q_a3 = ( L*L*yppb - 8.0*L*ypb + 20.0*yb - 20.0*y_targ) / (2.0*L*L*L);
        q_a4 = (-L*L*yppb + 7.0*L*ypb - 15.0*yb + 15.0*y_targ) / (L*L*L*L);
        q_a5 = ( L*L*yppb - 6.0*L*ypb + 12.0*yb - 12.0*y_targ) / (2.0*L*L*L*L*L);
    }

    int keep_from = 0;
    while (keep_from < br->n_pts && br->xs[keep_from] <= xb + 1e-12) keep_from++;
    int n_keep = br->n_pts - keep_from;

    int     next   = NS_EXTEND_PTS;
    int     newn   = next + 1 + n_keep;
    double *new_xs = (double*)malloc(newn * sizeof(double));
    double *new_ys = (double*)malloc(newn * sizeof(double));
    if (!new_xs || !new_ys) { free(new_xs); free(new_ys); return 0; }

    double span = xb - x_target;
    int idx = 0;
    for (int i = 0; i < next; i++) {
        double x = x_target + span * ((double)i / (double)next);
        double u = x - x_target;
        double y;
        if (use_quintic) {
            double u2 = u*u, u3 = u2*u;
            y = q_a0 + u3 * (q_a3 + u * (q_a4 + u * q_a5));
        } else {
            y = A0 + A1 * u + A3 * u * u * u;
        }
        new_xs[idx] = x;
        new_ys[idx] = y;
        idx++;
    }
    new_xs[idx] = xb;
    new_ys[idx] = yb;
    idx++;
    for (int i = keep_from; i < br->n_pts; i++) {
        new_xs[idx] = br->xs[i];
        new_ys[idx] = br->ys[i];
        idx++;
    }
    newn = idx;

    NS_Seg *new_segs = (NS_Seg*)malloc((newn - 1) * sizeof(NS_Seg));
    if (!new_segs) { free(new_xs); free(new_ys); return 0; }

    free(br->xs);
    free(br->ys);
    free(br->segs);
    br->xs    = new_xs;
    br->ys    = new_ys;
    br->segs  = new_segs;
    br->n_pts = newn;

    build_spline_segments(br->xs, br->ys, br->n_pts, br->segs);

    return 1;
}

static int find_seg(const double *xs, int n, double x)
{
    if (x <= xs[0])   return 0;
    if (x >= xs[n-1]) return n-2;
    int lo=0, hi=n-1;
    while (hi-lo > 1) {
        int mid=(lo+hi)>>1;
        if (x < xs[mid]) hi=mid; else lo=mid;
    }
    return lo;
}

double ns_eval(const NS_Spline *s, double x)
{
    if (!s || s->n_branches == 0) return NAN;
    if (s->n_branches == 1) {
        const NS_Branch *br = &s->branches[0];
        if (br->n_pts < 2) return NAN;
        double xlo = br->xs[0], xhi = br->xs[br->n_pts-1];
        if (x < xlo - NS_EDGE_CLAMP_TOL || x > xhi + NS_EDGE_CLAMP_TOL)
            return NAN;
        if (x < xlo) x = xlo;
        if (x > xhi) x = xhi;
        int i = find_seg(br->xs, br->n_pts, x);
        double dx = x - br->segs[i].x0;
        return br->segs[i].a + dx*(br->segs[i].b
                               + dx*(br->segs[i].c
                               + dx* br->segs[i].d));
    }
    double best_y    = NAN;
    double best_dist = DBL_MAX;
    for (int b = 0; b < s->n_branches; b++) {
        const NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        double xlo = br->xs[0], xhi = br->xs[br->n_pts-1];
        if (x < xlo - NS_EDGE_CLAMP_TOL || x > xhi + NS_EDGE_CLAMP_TOL) continue;
        double xc = x;
        if (xc < xlo) xc = xlo;
        if (xc > xhi) xc = xhi;
        double dist = fabs(br->t_mid - 0.5);
        if (dist < best_dist) {
            best_dist = dist;
            int    i  = find_seg(br->xs, br->n_pts, xc);
            double dx = xc - br->segs[i].x0;
            best_y = br->segs[i].a + dx*(br->segs[i].b
                                     + dx*(br->segs[i].c
                                     + dx* br->segs[i].d));
        }
    }
    return best_y;
}

double ns_eval_near(const NS_Spline *s, double x, double prev_y)
{
    if (!s || s->n_branches == 0) return NAN;
    if (s->n_branches == 1) {
        const NS_Branch *br = &s->branches[0];
        double xlo = br->xs[0], xhi = br->xs[br->n_pts-1];
        if (x < xlo - NS_EDGE_CLAMP_TOL || x > xhi + NS_EDGE_CLAMP_TOL)
            return NAN;
        if (x < xlo) x = xlo;
        if (x > xhi) x = xhi;
        int    i  = find_seg(br->xs, br->n_pts, x);
        double dx = x - br->segs[i].x0;
        return br->segs[i].a + dx*(br->segs[i].b
                               + dx*(br->segs[i].c
                               + dx* br->segs[i].d));
    }
    double best_y    = NAN;
    double best_dist = DBL_MAX;

    for (int b = 0; b < s->n_branches; b++) {
        const NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        double xlo = br->xs[0], xhi = br->xs[br->n_pts-1];
        if (x < xlo - NS_EDGE_CLAMP_TOL || x > xhi + NS_EDGE_CLAMP_TOL) continue;
        double xc = x;
        if (xc < xlo) xc = xlo;
        if (xc > xhi) xc = xhi;

        int    i  = find_seg(br->xs, br->n_pts, xc);
        double dx = xc - br->segs[i].x0;
        double y  = br->segs[i].a + dx*(br->segs[i].b
                                    + dx*(br->segs[i].c
                                    + dx* br->segs[i].d));
        double dist = fabs(y - prev_y);
        if (dist < best_dist) {
            best_dist = dist;
            best_y    = y;
        }
    }

    return best_y;
}

double ns_eval_left_edge(const NS_Spline *s)
{
    if (!s || s->n_branches == 0) return 0.0;

    double xlo_best = DBL_MAX;
    int    b_best   = -1;
    for (int b = 0; b < s->n_branches; b++) {
        if (s->branches[b].n_pts < 2) continue;
        if (s->branches[b].xs[0] < xlo_best) {
            xlo_best = s->branches[b].xs[0];
            b_best   = b;
        }
    }

    if (b_best < 0) return 0.0;
    const NS_Branch *br = &s->branches[b_best];
    double dx = 0.0;
    return br->segs[0].a;
}

double ns_eval_near_clamped(const NS_Spline *s, double x, double prev_y)
{
    if (!s || s->n_branches == 0) return 0.0;
    double xlo =  DBL_MAX;
    double xhi = -DBL_MAX;
    for (int b = 0; b < s->n_branches; b++) {
        const NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        if (br->xs[0]           < xlo) xlo = br->xs[0];
        if (br->xs[br->n_pts-1] > xhi) xhi = br->xs[br->n_pts-1];
    }
    double xc = x;
    if (xc < xlo) xc = xlo;
    if (xc > xhi) xc = xhi;
    return ns_eval_near(s, xc, prev_y);
}
int ns_eval_all(const NS_Spline *s, double x,
                double *y_out, int max_out)
{
    int found = 0;
    for (int b = 0; b < s->n_branches && found < max_out; b++) {
        const NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        double xlo = br->xs[0], xhi = br->xs[br->n_pts-1];
        if (x < xlo - NS_EDGE_CLAMP_TOL || x > xhi + NS_EDGE_CLAMP_TOL) continue;
        double xc = x;
        if (xc < xlo) xc = xlo;
        if (xc > xhi) xc = xhi;
        int    i  = find_seg(br->xs, br->n_pts, xc);
        double dx = xc - br->segs[i].x0;
        y_out[found++] = br->segs[i].a + dx*(br->segs[i].b
                                         + dx*(br->segs[i].c
                                         + dx* br->segs[i].d));
    }
    return found;
}

void ns_x_range(const NS_Spline *s, double *x_min, double *x_max)
{
    double lo=DBL_MAX, hi=-DBL_MAX;
    for (int b = 0; b < s->n_branches; b++) {
        const NS_Branch *br = &s->branches[b];
        if (br->n_pts < 2) continue;
        if (br->xs[0]          < lo) lo = br->xs[0];
        if (br->xs[br->n_pts-1] > hi) hi = br->xs[br->n_pts-1];
    }
    if (x_min) *x_min = lo;
    if (x_max) *x_max = hi;
}

double ns_accuracy_check(const NS_Spline *s, const NF_Curve *curve,
                          int n_check,
                          double *max_err_out, double *rms_err_out)
{
    if (n_check <= 0) n_check = 1000;
    double max_err = 0.0, sse = 0.0;
    int    cnt     = 0;
    double xlo = 0.0, xhi = 1.0;
    ns_x_range(s, &xlo, &xhi);

    for (int i = 0; i <= n_check; i++) {
        double t  = (double)i / n_check;
        NF_Point2 pt = nf_eval(curve, t);
        if (pt.x < xlo || pt.x > xhi) continue;

        double y_spline = ns_eval(s, pt.x);
        if (!isnan(y_spline)) {
            double err = fabs(pt.y - y_spline);
            if (err > max_err) max_err = err;
            sse += ns_sq(err);
            cnt++;
        }
    }

    double rms = (cnt > 0) ? sqrt(sse/cnt) : 0.0;
    if (max_err_out) *max_err_out = max_err;
    if (rms_err_out) *rms_err_out = rms;
    return max_err;
}

void ns_free(NS_Spline *s)
{
    if (!s) return;
    for (int b = 0; b < s->n_branches; b++) {
        free(s->branches[b].xs);
        free(s->branches[b].ys);
        free(s->branches[b].segs);
    }
    free(s->branches);
    free(s);
}

NS_Spline *ns_copy(const NS_Spline *src)
{
    if (!src) return NULL;
    NS_Spline *dst = (NS_Spline*)ns_calloc(1, sizeof(NS_Spline));
    dst->n_branches = src->n_branches;
    dst->branches   = (NS_Branch*)ns_calloc(src->n_branches, sizeof(NS_Branch));

    for (int b = 0; b < src->n_branches; b++) {
        const NS_Branch *sb = &src->branches[b];
        NS_Branch       *db = &dst->branches[b];

        db->n_pts  = sb->n_pts;
        db->t_mid  = sb->t_mid;

        if (sb->n_pts == 0) {
            db->xs   = NULL;
            db->ys   = NULL;
            db->segs = NULL;
            continue;
        }

        db->xs   = (double*)ns_malloc(sb->n_pts * sizeof(double));
        db->ys   = (double*)ns_malloc(sb->n_pts * sizeof(double));
        db->segs = (NS_Seg*) ns_malloc((sb->n_pts - 1) * sizeof(NS_Seg));

        memcpy(db->xs,   sb->xs,   sb->n_pts * sizeof(double));
        memcpy(db->ys,   sb->ys,   sb->n_pts * sizeof(double));
        memcpy(db->segs, sb->segs, (sb->n_pts - 1) * sizeof(NS_Seg));
    }

    return dst;
}

static int write_spline(FILE *f, const NS_Spline *s, double *cksum)
{
    if (fprintf(f, "n_branches %d\n", s->n_branches) < 0) return -1;
    *cksum += s->n_branches;
    for (int b = 0; b < s->n_branches; b++) {
        const NS_Branch *br = &s->branches[b];
        if (fprintf(f, "branch %d n_pts %d t_mid %.17g\n",
                    b, br->n_pts, br->t_mid) < 0) return -1;
        *cksum += br->n_pts + br->t_mid;
        for (int i = 0; i < br->n_pts; i++) {
            if (fprintf(f, "  %.17g %.17g\n", br->xs[i], br->ys[i]) < 0)
                return -1;
            *cksum += br->xs[i] + br->ys[i];
        }
    }
    return 0;
}

static NS_Spline *read_spline(FILE *f, double *cksum)
{
    NS_Spline *s = (NS_Spline*)ns_calloc(1, sizeof(NS_Spline));
    char key[64];

    if (fscanf(f, "%63s %d", key, &s->n_branches) != 2) goto err;
    *cksum += s->n_branches;
    s->branches = (NS_Branch*)ns_calloc(s->n_branches, sizeof(NS_Branch));

    for (int b = 0; b < s->n_branches; b++) {
        NS_Branch *br = &s->branches[b];
        int bnum;
        if (fscanf(f, "%63s %d %63s %d %63s %lf",
                   key, &bnum, key, &br->n_pts, key, &br->t_mid) != 6)
            goto err;
        *cksum += br->n_pts + br->t_mid;

        if (br->n_pts == 0) {
            br->xs = br->ys = NULL; br->segs = NULL;
            continue;
        }
        br->xs   = (double*)ns_malloc(br->n_pts * sizeof(double));
        br->ys   = (double*)ns_malloc(br->n_pts * sizeof(double));
        br->segs = (NS_Seg *)ns_malloc((br->n_pts - 1) * sizeof(NS_Seg));

        for (int i = 0; i < br->n_pts; i++) {
            if (fscanf(f, "%lf %lf", &br->xs[i], &br->ys[i]) != 2)
                goto err;
            *cksum += br->xs[i] + br->ys[i];
        }
        build_spline_segments(br->xs, br->ys, br->n_pts, br->segs);
    }
    return s;

err:
    ns_free(s);
    return NULL;
}

void curve_ema_init(CurveEMA *e, double alpha)
{
    curve_ema_init2(e, alpha, alpha, 0.0, 0.1, 2.0);
}

void curve_ema_init2(CurveEMA *e,
                     double alpha,
                     double alpha_lo,
                     double x_alpha_boundary,
                     double y_clip_lo,
                     double y_clip_hi)
{
    e->alpha            = alpha;
    e->alpha_lo         = alpha_lo;
    e->x_alpha_boundary = x_alpha_boundary;
    e->y_clip_lo        = y_clip_lo;
    e->y_clip_hi        = y_clip_hi;
    e->count            = 0;
    e->warmup_cycles    = 4;
    
    for (int i = 0; i < CURVE_EMA_PTS; i++) {
        e->xs[i] = (double)i / (CURVE_EMA_PTS - 1);
        e->ys[i] = 1.0;
    }
}

void curve_ema_update(CurveEMA *e, const NS_Spline *s)
{
    if (!s) return;
    double bnd   = e->x_alpha_boundary;
    double width = (bnd > 0.0) ? fmax(0.02, bnd * 0.20) : 0.0;

    for (int i = 0; i < CURVE_EMA_PTS; i++) {
        double xi = e->xs[i];
        double local_alpha;
        if (bnd <= 0.0 || e->alpha_lo == e->alpha) {
            local_alpha = e->alpha;
        } else {
            double lo_edge = bnd - width * 0.5;
            double hi_edge = bnd + width * 0.5;
            if (xi <= lo_edge) {
                local_alpha = e->alpha_lo;
            } else if (xi >= hi_edge) {
                local_alpha = e->alpha;
            } else {
                double t = (xi - lo_edge) / (hi_edge - lo_edge);
                double blend = 0.5 * (1.0 - cos(t * 3.14159265358979323846));
                local_alpha = e->alpha_lo + blend * (e->alpha - e->alpha_lo);
            }
        }
        if (e->warmup_cycles > 0 && e->count > 0 && e->count <= e->warmup_cycles)
            local_alpha = e->alpha;

        double prev_y = (i > 0) ? e->ys[i-1] : 1.0;
        double new_y  = ns_eval_near_clamped(s, xi, prev_y);
        if (isnan(new_y)) new_y = 1.0;

        if (e->count == 0) {
            e->ys[i] = fmax(e->y_clip_lo, fmin(e->y_clip_hi, new_y));
        } else {
            e->ys[i] = local_alpha * new_y + (1.0 - local_alpha) * e->ys[i];
        }
    }

    e->count++;
}
void curve_ema_enforce_monotone(CurveEMA *e, double x_start)
{
    for (int i = CURVE_EMA_PTS - 2; i >= 0; i--) {
        if (e->xs[i] < x_start) break;
        if (e->ys[i] < e->ys[i+1])
            e->ys[i] = e->ys[i+1];
    }
}

static double ema_interp(const CurveEMA *e, double x)
{
    if (x <= e->xs[0])               return e->ys[0];
    if (x >= e->xs[CURVE_EMA_PTS-1]) return e->ys[CURVE_EMA_PTS-1];

    double step = e->xs[1] - e->xs[0];
    int i = (int)((x - e->xs[0]) / step);
    if (i >= CURVE_EMA_PTS - 1) i = CURVE_EMA_PTS - 2;

    double t = (x - e->xs[i]) / step;
    double y0 = e->ys[i],   y1 = e->ys[i+1];
    double m0, m1;

    if (i > 0)
        m0 = 0.5 * (e->ys[i+1] - e->ys[i-1]);
    else
        m0 = e->ys[i+1] - e->ys[i];

    if (i < CURVE_EMA_PTS - 2)
        m1 = 0.5 * (e->ys[i+2] - e->ys[i]);
    else
        m1 = e->ys[i+1] - e->ys[i];

    double t2 = t*t, t3 = t2*t;
    double h00 =  2*t3 - 3*t2 + 1;
    double h10 =    t3 - 2*t2 + t;
    double h01 = -2*t3 + 3*t2;
    double h11 =    t3 -   t2;

    return h00*y0 + h10*m0 + h01*y1 + h11*m1;
}

double curve_ema_get_y(const CurveEMA *e, double x)
{
    if (!e) return 1.0;
    return ema_interp(e, x);
}

double get_mag_correction_ema(const CurveEMA *e, double x, double *prev_y)
{
    (void)prev_y;
    if (!e || x <= 0.0) return 1.0;
    return ema_interp(e, x);
}

double get_phase_correction_ema(const CurveEMA *e, double x, double *prev_y)
{
    (void)prev_y;
    if (!e || x <= 0.0) return ema_interp(e, 0.0);
    return ema_interp(e, x);
}

#define CORRECTION_FILE_VERSION_V2  2

static int write_curve_ema(FILE *f, const CurveEMA *e, double *cksum)
{
    if (fprintf(f, "curve_ema_alpha %.17g\n",    e->alpha)            < 0) return -1;
    if (fprintf(f, "curve_ema_alpha_lo %.17g\n", e->alpha_lo)         < 0) return -1;
    if (fprintf(f, "curve_ema_x_bnd %.17g\n",   e->x_alpha_boundary) < 0) return -1;
    if (fprintf(f, "curve_ema_clip %.17g %.17g\n", e->y_clip_lo, e->y_clip_hi) < 0) return -1;
    if (fprintf(f, "curve_ema_count %d\n",       e->count)            < 0) return -1;
    if (fprintf(f, "curve_ema_warmup %d\n",      e->warmup_cycles)   < 0) return -1;
    *cksum += e->warmup_cycles;
    if (fprintf(f, "curve_ema_pts %d\n", CURVE_EMA_PTS)               < 0) return -1;
    *cksum += e->alpha + e->alpha_lo + e->x_alpha_boundary
           +  e->y_clip_lo + e->y_clip_hi + e->count;
    for (int i = 0; i < CURVE_EMA_PTS; i++) {
        if (fprintf(f, "  %.17g\n", e->ys[i]) < 0) return -1;
        *cksum += e->ys[i];
    }
    return 0;
}

static int read_curve_ema(FILE *f, CurveEMA *e, double *cksum)
{
    char key[64]; double alpha, alpha_lo, x_bnd, clip_lo, clip_hi; int count, pts;
    if (fscanf(f, "%63s %lf", key, &alpha)           != 2) return -1;
    if (fscanf(f, "%63s %lf", key, &alpha_lo)        != 2) return -1;
    if (fscanf(f, "%63s %lf", key, &x_bnd)           != 2) return -1;
    if (fscanf(f, "%63s %lf %lf", key, &clip_lo, &clip_hi) != 3) return -1;
    if (fscanf(f, "%63s %d",  key, &count)           != 2) return -1;
    int warmup;
    if (fscanf(f, "%63s %d",  key, &warmup)          != 2) return -1;
    if (fscanf(f, "%63s %d",  key, &pts)             != 2) return -1;
    if (pts != CURVE_EMA_PTS) return -1;

    curve_ema_init2(e, alpha, alpha_lo, x_bnd, clip_lo, clip_hi);
    e->count         = count;
    e->warmup_cycles = warmup;
    *cksum += alpha + alpha_lo + x_bnd + clip_lo + clip_hi + count + warmup;

    for (int i = 0; i < CURVE_EMA_PTS; i++) {
        if (fscanf(f, "%lf", &e->ys[i]) != 1) return -1;
        *cksum += e->ys[i];
    }
    return 0;
}

int WriteCorrectionFileV2(const char      *filename,
                          const NS_Spline *m_spline,
                          const CurveEMA  *m_ema,
                          const NS_Spline *c_spline,
                          const CurveEMA  *c_ema,
                          const NS_Spline *s_spline,
                          const CurveEMA  *s_ema)
{
    if (!filename || !m_spline || !c_spline || !s_spline) return -1;
    if (!m_ema    || !c_ema    || !s_ema)                 return -1;

    FILE *f = fopen(filename, "w");
    if (!f) return -1;

    double cksum = 0.0;

    if (fprintf(f, "correction_file_version %d\n",
                CORRECTION_FILE_VERSION_V2) < 0) goto err;

    const char        *names[3] = {"MAG", "COS", "SIN"};
    const NS_Spline   *splines[3] = {m_spline, c_spline, s_spline};
    const CurveEMA    *emas[3]    = {m_ema,    c_ema,    s_ema};

    for (int ci = 0; ci < 3; ci++) {
        if (fprintf(f, "curve %s\n", names[ci]) < 0) goto err;
        if (write_curve_ema(f, emas[ci], &cksum) != 0) goto err;
        if (write_spline(f, splines[ci], &cksum) != 0) goto err;
    }

    if (fprintf(f, "checksum %.17g\n", cksum) < 0) goto err;
    fclose(f);
    return 0;

err:
    fclose(f);
    return -1;
}

int ReadCorrectionFileV2(const char  *filename,
                         NS_Spline  **m_spline, CurveEMA *m_ema,
                         NS_Spline  **c_spline, CurveEMA *c_ema,
                         NS_Spline  **s_spline, CurveEMA *s_ema)
{
    if (!filename || !m_spline || !c_spline || !s_spline) return -1;
    if (!m_ema    || !c_ema    || !s_ema)                 return -1;

    *m_spline = *c_spline = *s_spline = NULL;

    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    double cksum = 0.0;
    char key[64]; int version;

    if (fscanf(f, "%63s %d", key, &version) != 2) goto err;
    if (version != CORRECTION_FILE_VERSION_V2) {
        fprintf(stderr,
            "ReadCorrectionFileV2: file is version %d, expected %d.\n"
            "  Version 1 files (CalAvgState format) are not supported.\n"
            "  Please recalibrate to generate a version 2 file.\n",
            version, CORRECTION_FILE_VERSION_V2);
        goto err;
    }

    const char  *curve_names[3] = {"MAG", "COS", "SIN"};
    NS_Spline  **splines[3]     = {m_spline, c_spline, s_spline};
    CurveEMA    *emas[3]        = {m_ema,    c_ema,    s_ema};

    for (int ci = 0; ci < 3; ci++) {
        char curve_tag[16];
        if (fscanf(f, "%63s %15s", key, curve_tag) != 2)           goto err;
        if (strcmp(curve_tag, curve_names[ci]) != 0)                goto err;
        if (read_curve_ema(f, emas[ci], &cksum) != 0)              goto err;
        *splines[ci] = read_spline(f, &cksum);
        if (!*splines[ci])                                          goto err;
    }

    double stored_cksum;
    if (fscanf(f, "%63s %lf", key, &stored_cksum) != 2)            goto err;
    if (fabs(stored_cksum - cksum) > fabs(cksum) * 1e-9 + 1e-9)   goto err;

    fclose(f);
    return 0;

err:
    fclose(f);
    ns_free(*m_spline); *m_spline = NULL;
    ns_free(*c_spline); *c_spline = NULL;
    ns_free(*s_spline); *s_spline = NULL;
    return -1;
}
