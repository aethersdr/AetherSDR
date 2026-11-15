/*  phrot.h

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

The author can be reached by email at  warren@pratt.one
*/

/*  Acknowledgment

    The auto-optimizer and waveform asymmetry display subsystem in this file
    were developed with the assistance of Claude (Anthropic), an AI assistant,
    in collaboration with Warren Pratt, NR0V.  The algorithms, design decisions,
    and final implementation are the original work of Warren Pratt.  Claude
    contributed to algorithm design, code drafting, debugging, and documentation
    during an iterative development process.

    Claude does not hold copyright.  All copyright in this file remains with
    Warren Pratt, NR0V, as stated above.
*/

#ifndef _phrot_h
#define _phrot_h

typedef struct _phrot
{
    int     reverse;
    int     run;
    int     size;
    double *in;
    double *out;
    int     rate;
    double  fc;
    int     nstages;
    double  b0, b1, a1;
    double *x0, *x1;
    double *y0, *y1;
    CRITICAL_SECTION cs_update;
    int     optim_interval;
    int     optim_samp_count;
    double  coeff_attack;
    double  coeff_decay;
    double  coeff_asym;
    double  coeff_asym_fast;
    double  env_pos;
    double  env_neg;
    double  asym_metric;
    double  asym_fast;
    double  in_env_pos;
    double  in_env_neg;
    double  in_asym_metric;
    int     autoMode;
    double  auto_fc;
    double  auto_asym_best;
    double  auto_step;
    int     auto_direction;
    int     auto_reject_count;
    int     auto_converged;
    int     ab_phase;
    double  fc_A;
    double  fc_B;
    double  asym_A;
} phrot, *PHROT;

extern void calc_phrot   (PHROT a);
extern void decalc_phrot (PHROT a);
extern void flush_phrot  (PHROT a);
extern PHROT create_phrot  (int run, int size, double* in, double* out,
                             int rate, double fc, int nstages);
extern void  destroy_phrot (PHROT a);
extern void xphrot (PHROT a);
extern void setBuffers_phrot   (PHROT a, double* in, double* out);
extern void setSamplerate_phrot(PHROT a, int rate);
extern void setSize_phrot      (PHROT a, int size);

__declspec(dllexport) void SetTXAPHROTRun     (int channel, int run);
__declspec(dllexport) void SetTXAPHROTCorner  (int channel, double frequency);
__declspec(dllexport) void SetTXAPHROTNstages (int channel, int nstages);
__declspec(dllexport) void SetTXAPHROTReverse (int channel, int reverse);
__declspec(dllexport) void SetTXAPHROTAutoMode(int channel, int autoMode);
__declspec(dllexport) void SetTXAPHROTAutoReset(int channel);
__declspec(dllexport) void GetTXAPHROTAsymmetry(int channel,
                                                 double* in_pos,
                                                 double* in_neg,
                                                 double* in_ratio,
                                                 double* out_pos,
                                                 double* out_neg,
                                                 double* out_ratio,
                                                 double* current_fc,
                                                 double* auto_step);

#endif  // _phrot_h
