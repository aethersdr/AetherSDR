/*  nurbs_spline.h

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

#define _CRT_SECURE_NO_WARNINGS

#ifndef NURBS_SPLINE_H
#define NURBS_SPLINE_H

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#include "nurbs_fit.h"

typedef struct {
    double x0;
    double a, b, c, d;
} NS_Seg;

typedef struct {
    int      n_pts;
    double  *xs;
    double  *ys;
    NS_Seg  *segs;
    double   t_mid;
} NS_Branch;

typedef struct {
    int        n_branches;
    NS_Branch *branches;
} NS_Spline;

#ifndef NS_DEFAULT_PTS
#  define NS_DEFAULT_PTS 256
#endif

#ifndef NS_SAMPLES_PER_CTRL
#  define NS_SAMPLES_PER_CTRL 8
#endif

NS_Spline *ns_build(const NF_Curve    *curve,
                    const NF_FitResult *result,
                    int                 n_pts);

int ns_extend_left(NS_Spline *s,
                   double x_target,
                   double x_anchor,
                   double bound_frac,
                   double y_lo_clamp,
                   double y_hi_clamp);

double ns_eval(const NS_Spline *s, double x);

double ns_eval_near(const NS_Spline *s, double x, double prev_y);

double ns_eval_near_clamped(const NS_Spline *s, double x, double prev_y);

// This function not called.
double ns_eval_left_edge(const NS_Spline *s);

// This function not called.
int ns_eval_all(const NS_Spline *s, double x,
                double *y_out, int max_out);

void ns_x_range(const NS_Spline *s, double *x_min, double *x_max);

double ns_accuracy_check(const NS_Spline *s, const NF_Curve *curve,
                          int n_check,
                          double *max_err, double *rms_err);

void ns_free(NS_Spline *s);

NS_Spline *ns_copy(const NS_Spline *src);


#define CURVE_EMA_PTS  256

typedef struct _CurveEMA {
    double xs[CURVE_EMA_PTS];
    double ys[CURVE_EMA_PTS];
    double alpha;
    double alpha_lo;
    double x_alpha_boundary;
    double y_clip_lo;
    double y_clip_hi;
    int    count;
    int    warmup_cycles;
} CurveEMA;

// This function not called.
void curve_ema_init(CurveEMA *e, double alpha);

void curve_ema_init2(CurveEMA *e,
                     double alpha,
                     double alpha_lo,
                     double x_alpha_boundary,
                     double y_clip_lo,
                     double y_clip_hi);

// This function not called.
double curve_ema_get_y(const CurveEMA *e, double x);

void curve_ema_update(CurveEMA *e, const NS_Spline *s);

// This function not called.
void curve_ema_enforce_monotone(CurveEMA *e, double x_start);

double get_mag_correction_ema(const CurveEMA *e, double x, double *prev_y);

double get_phase_correction_ema(const CurveEMA *e, double x, double *prev_y);

int WriteCorrectionFileV2(const char      *filename,
                          const NS_Spline *m_spline,
                          const CurveEMA  *m_ema,
                          const NS_Spline *c_spline,
                          const CurveEMA  *c_ema,
                          const NS_Spline *s_spline,
                          const CurveEMA  *s_ema);

int ReadCorrectionFileV2(const char  *filename,
                         NS_Spline  **m_spline, CurveEMA *m_ema,
                         NS_Spline  **c_spline, CurveEMA *c_ema,
                         NS_Spline  **s_spline, CurveEMA *s_ema);

#endif
