/*  nurbs_fit.h

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


#ifndef NURBS_FIT_H
#define NURBS_FIT_H

#include <stddef.h>

typedef struct {
    double x, y;
} NF_Point2;

typedef struct {
    int     degree;          
    int     n_ctrl;          
    double *knots;           
    double *ctrl_wx;         
    double *ctrl_wy;         
    double *weights;         
} NF_Curve;

#define NF_FIT_OK            0    
#define NF_FIT_OUTLIERS      1    
#define NF_FIT_ADAPTED       2    
#define NF_FIT_REPARAM_SKIP  4    
#define NF_FIT_CV_MARGINAL   8    
#define NF_FIT_PRE_FILTERED  0x10000  
#define NF_FIT_DIRECT        0x20000  
#define NF_FIT_NONMONOTONE   0x40000  
 
#define NF_FIT_BAD           0x3F0   
#define NF_FIT_BAD_CONDNUM   0x10    
#define NF_FIT_BAD_NOCONV    0x20    
#define NF_FIT_BAD_RANGE     0x40    
#define NF_FIT_BAD_TOOFEW    0x80    
#define NF_FIT_BAD_OVERFIT   0x100  

#define NF_FIT_BAD_BOUNDS    0x200  
 
typedef struct {
    int    quality;           
    double rms;               
    double rms_outlier;       
    int    n_outliers;        
    int    n_ctrl_final;      
    int    n_ctrl_initial;    
    double condition_number; 
    double cv_score;         
    int    ordering_used;     
    double spearman_rho;      
    int    fold_detected;     
    double fold_x_end;       
} NF_FitResult;

#define NF_ORDER_AUTO   0    
#define NF_ORDER_BY_X   1    
#define NF_ORDER_NN     2    

typedef struct {
    int    degree;
    int    n_ctrl;
    int    n_ctrl_max;
    int    ordering_mode;
    double spearman_threshold;
    int    nn_search_k;
    int    outlier_iters;
    double outlier_sigma;
    double outlier_min_fraction;
    int    local_outlier_iters;
    double local_outlier_sigma;
    int    local_outlier_bands;
    int        pin_start;
    NF_Point2  start_pt;
    int        pin_end;
    NF_Point2  end_pt;
    int        pin_end_horiz;
    int        pin_end_flat;
    int        pin_end_flat2;
    int    adaptive_iters;
    double adaptive_threshold;
    double cv_fraction;
    double cv_overfit_ratio;
    double cv_fatal_ratio;
    int    min_pts_per_ctrl;
    double y_min;
    double y_max;
    int    direct_n_segments;
    double direct_monotone_x_start;
    double pre_filter_x_min;
    double pre_filter_y_max;
    int    reparam_iters;
    double x_weight_x0;
    double x_weight_min;
    int    fold_detect;
    int    irls_iters;
    double irls_epsilon;
} NF_Config;

void nf_default_config(NF_Config *cfg);

// This function no longer called.
NF_Curve *nf_fit_direct(const NF_Point2 *pts,
                         int              n_pts,
                         const NF_Config *cfg,
                         NF_FitResult    *result_out);

NF_Curve *nf_fit(const NF_Point2 *pts, int n_pts,
                 const NF_Config *cfg,
                 NF_FitResult    *result);

NF_Point2 nf_eval(const NF_Curve *c, double t);

void nf_curve_free(NF_Curve *c);
 
double nf_spearman(const NF_Point2 *pts, int n);

// This function not called.
double nf_compute_rms(const NF_Curve *c,
                      const NF_Point2 *pts, int n_pts,
                      const double *t_params);   

// This function not called.
NF_Point2 *nf_sample(const NF_Curve *c, int n_samples);

// This function not called.
int nf_curve_write(const NF_Curve *c, const char *path);

// This function not called.
NF_Curve *nf_curve_read(const char *path);

#endif  
