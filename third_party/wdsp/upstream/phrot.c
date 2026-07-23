/*  phrot.c

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

#include "comm.h"

#define PHROT_FC_MIN        50.0
#define PHROT_FC_MAX      2000.0
#define PHROT_FC_DEFAULT   338.0

#define PHROT_STEP_INIT     50.0
#define PHROT_STEP_MIN       5.0
#define PHROT_STEP_SHRINK    0.75
#define PHROT_IMPROVE_THRESH 0.005
#define PHROT_CONVERGE_COUNT 4

#define PHROT_ATTACK_SECS    0.010
#define PHROT_DECAY_SECS     0.200
#define PHROT_ASYM_SECS     30.0
#define PHROT_ASYM_FAST_SECS 1.0

#define PHROT_OPTIM_SECS    15.0

void calc_phrot(PHROT a)
{
    double g, dt;
    a->x0 = (double*)malloc0(a->nstages * sizeof(double));
    a->x1 = (double*)malloc0(a->nstages * sizeof(double));
    a->y0 = (double*)malloc0(a->nstages * sizeof(double));
    a->y1 = (double*)malloc0(a->nstages * sizeof(double));

    g     = tan(PI * a->fc / (double)a->rate);
    a->b0 = (g - 1.0) / (g + 1.0);
    a->b1 = 1.0;
    a->a1 = a->b0;

    a->optim_interval = (int)(PHROT_OPTIM_SECS * (double)a->rate);

    dt                 = (double)a->size / (double)a->rate;
    a->coeff_attack    = exp(-dt / PHROT_ATTACK_SECS);
    a->coeff_decay     = exp(-dt / PHROT_DECAY_SECS);
    a->coeff_asym      = exp(-dt / PHROT_ASYM_SECS);
    a->coeff_asym_fast = exp(-dt / PHROT_ASYM_FAST_SECS);
}

void decalc_phrot(PHROT a)
{
    _aligned_free(a->y1);
    _aligned_free(a->y0);
    _aligned_free(a->x1);
    _aligned_free(a->x0);
}

void flush_phrot(PHROT a)
{
    memset(a->x0, 0, a->nstages * sizeof(double));
    memset(a->x1, 0, a->nstages * sizeof(double));
    memset(a->y0, 0, a->nstages * sizeof(double));
    memset(a->y1, 0, a->nstages * sizeof(double));
    a->env_pos     = 0.0;
    a->env_neg     = 0.0;
    a->asym_metric = 0.0;
    a->asym_fast   = 0.0;
    a->in_env_pos     = 0.0;
    a->in_env_neg     = 0.0;
    a->in_asym_metric = 0.0;
    a->optim_samp_count = 0;
}

static void recalc_fc(PHROT a, double new_fc)
{
    double g;
    if (new_fc < PHROT_FC_MIN) new_fc = PHROT_FC_MIN;
    if (new_fc > PHROT_FC_MAX) new_fc = PHROT_FC_MAX;
    a->fc = new_fc;
    g     = tan(PI * a->fc / (double)a->rate);
    a->b0 = (g - 1.0) / (g + 1.0);
    a->b1 = 1.0;
    a->a1 = a->b0;
    memset(a->x0, 0, a->nstages * sizeof(double));
    memset(a->x1, 0, a->nstages * sizeof(double));
    memset(a->y0, 0, a->nstages * sizeof(double));
    memset(a->y1, 0, a->nstages * sizeof(double));
}

static void update_metrics(
    int     size,
    double *buf,
    double  coeff_attack,
    double  coeff_decay,
    double  coeff_asym,
    double  coeff_asym_fast,
    double *env_pos,
    double *env_neg,
    double *asym_metric,
    double *asym_fast)
{
    int    i;
    double samp, blk_pos, blk_neg, cp, cn, raw_asym, denom;

    blk_pos = 0.0;
    blk_neg = 0.0;

    for (i = 0; i < size; i++)
    {
        samp = buf[2 * i];
        if (samp >  blk_pos) blk_pos =  samp;
        if (samp < -blk_neg) blk_neg = -samp;
    }
    cp       = (blk_pos > *env_pos) ? coeff_attack : coeff_decay;
    cn       = (blk_neg > *env_neg) ? coeff_attack : coeff_decay;
    *env_pos = cp * (*env_pos) + (1.0 - cp) * blk_pos;
    *env_neg = cn * (*env_neg) + (1.0 - cn) * blk_neg;
    denom     = *env_pos + *env_neg;
    raw_asym  = (denom > 1e-20) ? fabs(*env_pos - *env_neg) / denom : 0.0;
    *asym_metric = coeff_asym      * (*asym_metric) + (1.0 - coeff_asym)      * raw_asym;
    *asym_fast   = coeff_asym_fast * (*asym_fast)   + (1.0 - coeff_asym_fast) * raw_asym;
}

static void optimizer_step(PHROT a)
{
    double asym_B, diff;
    if (a->auto_asym_best < 0.0)
    {
        a->auto_asym_best    = a->asym_fast;
        a->auto_fc           = a->fc;
        a->auto_direction    = -1;
        a->auto_step         = PHROT_STEP_INIT;
        a->auto_reject_count = 0;
        a->auto_converged    = 0;
        a->fc_A              = a->fc;
        a->asym_A            = a->asym_fast;
        a->fc_B              = a->fc;
        a->ab_phase          = 0;
    }
    if (a->ab_phase == 0)
    {
        a->asym_A = a->asym_fast;
        a->fc_A   = a->fc;
        a->fc_B = a->auto_fc + a->auto_direction * a->auto_step;
        if (a->fc_B < PHROT_FC_MIN) { a->fc_B = PHROT_FC_MIN; a->auto_direction =  1; }
        if (a->fc_B > PHROT_FC_MAX) { a->fc_B = PHROT_FC_MAX; a->auto_direction = -1; }
        a->ab_phase = 1;
        recalc_fc(a, a->fc_B);
    }
    else
    {
        asym_B = a->asym_fast;
        diff   = a->asym_A - asym_B;
        if (diff > a->asym_A * PHROT_IMPROVE_THRESH)
        {
            a->auto_fc           = a->fc_B;
            a->auto_asym_best    = asym_B;
            a->auto_step        *= PHROT_STEP_SHRINK;
            if (a->auto_step < PHROT_STEP_MIN) a->auto_step = PHROT_STEP_MIN;
            a->auto_reject_count = 0;
        }
        else if (-diff > a->asym_A * PHROT_IMPROVE_THRESH)
        {
            a->auto_fc           = a->fc_A;
            a->auto_asym_best    = a->asym_A;
            a->auto_direction    = -a->auto_direction;
            a->auto_step        *= PHROT_STEP_SHRINK;
            if (a->auto_step < PHROT_STEP_MIN) a->auto_step = PHROT_STEP_MIN;
            a->auto_reject_count = 0;
        }
        else
        {
            a->auto_direction = -a->auto_direction;
            a->auto_reject_count++;
            if (a->auto_reject_count >= PHROT_CONVERGE_COUNT)
            {
                a->auto_step        *= PHROT_STEP_SHRINK;
                if (a->auto_step < PHROT_STEP_MIN) a->auto_step = PHROT_STEP_MIN;
                a->auto_reject_count = 0;
            }
        }
        if (a->auto_step <= PHROT_STEP_MIN)
        {
            recalc_fc(a, a->auto_fc);
            a->auto_converged = 1;
            return;
        }
        recalc_fc(a, a->auto_fc);
        a->ab_phase = 0;
    }
}

PHROT create_phrot(int run, int size, double* in, double* out,
                   int rate, double fc, int nstages)
{
    PHROT a = (PHROT)malloc0(sizeof(phrot));

    a->reverse  = 0;
    a->run      = run;
    a->size     = size;
    a->in       = in;
    a->out      = out;
    a->rate     = rate;
    a->fc = (fc < PHROT_FC_MIN) ? PHROT_FC_MIN :
            (fc > PHROT_FC_MAX) ? PHROT_FC_MAX : fc;
    a->nstages = (nstages < 1) ? 1 : nstages;
    a->env_pos     = 0.0;
    a->env_neg     = 0.0;
    a->asym_metric = 0.0;
    a->asym_fast   = 0.0;
    a->in_env_pos     = 0.0;
    a->in_env_neg     = 0.0;
    a->in_asym_metric = 0.0;
    a->autoMode          = 0;
    a->auto_fc           = a->fc;
    a->auto_asym_best    = -1.0;
    a->auto_step         = PHROT_STEP_INIT;
    a->auto_direction    = 1;
    a->auto_reject_count = 0;
    a->auto_converged    = 0;
    a->ab_phase          = 0;
    a->fc_A              = a->fc;
    a->fc_B              = a->fc;
    a->asym_A            = 0.0;
    a->optim_samp_count  = 0;
    InitializeCriticalSectionAndSpinCount(&a->cs_update, 2500);
    calc_phrot(a);
    return a;
}

void destroy_phrot(PHROT a)
{
    decalc_phrot(a);
    DeleteCriticalSection(&a->cs_update);
    _aligned_free(a);
}

void xphrot(PHROT a)
{
    int i, n;
    EnterCriticalSection(&a->cs_update);
    if (a->reverse)
        for (i = 0; i < a->size; i++)
            a->in[2 * i] = -a->in[2 * i];
    if (a->run)
    {
        {
            double in_fast_scratch = a->in_asym_metric;
            update_metrics(a->size, a->in,
                a->coeff_attack, a->coeff_decay, a->coeff_asym, a->coeff_asym_fast,
                &a->in_env_pos, &a->in_env_neg,
                &a->in_asym_metric, &in_fast_scratch);
        }
        for (i = 0; i < a->size; i++)
        {
            a->x0[0] = a->in[2 * i];
            for (n = 0; n < a->nstages; n++)
            {
                if (n > 0) a->x0[n] = a->y0[n - 1];
                a->y0[n] = a->b0 * a->x0[n]
                         + a->b1 * a->x1[n]
                         - a->a1 * a->y1[n];
                a->y1[n] = a->y0[n];
                a->x1[n] = a->x0[n];
            }
            a->out[2 * i] = a->y0[a->nstages - 1];
        }
        update_metrics(a->size, a->out,
            a->coeff_attack, a->coeff_decay, a->coeff_asym, a->coeff_asym_fast,
            &a->env_pos, &a->env_neg,
            &a->asym_metric, &a->asym_fast);
        if (a->autoMode && !a->auto_converged)
        {
            a->optim_samp_count += a->size;
            if (a->optim_samp_count >= a->optim_interval)
            {
                a->optim_samp_count = 0;
                optimizer_step(a);
            }
        }
    }
    else if (a->out != a->in)
    {
        memcpy(a->out, a->in, a->size * sizeof(complex));
    }
    LeaveCriticalSection(&a->cs_update);
}

void setBuffers_phrot(PHROT a, double* in, double* out)
{
    a->in  = in;
    a->out = out;
}

void setSamplerate_phrot(PHROT a, int rate)
{
    decalc_phrot(a);
    a->rate = rate;
    calc_phrot(a);
    a->optim_samp_count = 0;
}

void setSize_phrot(PHROT a, int size)
{
    decalc_phrot(a);
    a->size = size;
    calc_phrot(a);
    flush_phrot(a);
}


/********************************************************************************************************
*                                                                                                       *
*                                       TXA Properties                                                  *
*                                                                                                       *
********************************************************************************************************/

PORT
void SetTXAPHROTRun(int channel, int run)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    a->run = run;
    if (a->run) flush_phrot(a);
    LeaveCriticalSection(&a->cs_update);
}

PORT
void SetTXAPHROTCorner(int channel, double frequency)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    if (frequency < PHROT_FC_MIN) frequency = PHROT_FC_MIN;
    if (frequency > PHROT_FC_MAX) frequency = PHROT_FC_MAX;
    decalc_phrot(a);
    a->fc = frequency;
    calc_phrot(a);
    if (!a->auto_converged)
    {
        a->auto_fc        = a->fc;
        a->auto_asym_best = -1.0;
        a->ab_phase       = 0;
    }
    LeaveCriticalSection(&a->cs_update);
}

PORT
void SetTXAPHROTNstages(int channel, int nstages)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    decalc_phrot(a);
    a->nstages = (nstages < 1) ? 1 : nstages;
    calc_phrot(a);
    LeaveCriticalSection(&a->cs_update);
}

PORT
void SetTXAPHROTReverse(int channel, int reverse)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    a->reverse = reverse;
    LeaveCriticalSection(&a->cs_update);
}

PORT
void SetTXAPHROTAutoMode(int channel, int autoMode)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    a->autoMode = autoMode;
    if (a->autoMode)
    {
        a->auto_fc           = a->fc;
        a->auto_asym_best    = -1.0;
        a->auto_step         = PHROT_STEP_INIT;
        a->auto_direction    = -1;
        a->auto_reject_count = 0;
        a->auto_converged    = 0;
        a->ab_phase          = 0;
        a->fc_A              = a->fc;
        a->fc_B              = a->fc;
        a->asym_A            = 0.0;
        a->optim_samp_count  = 0;
    }
    LeaveCriticalSection(&a->cs_update);
}

PORT
void SetTXAPHROTAutoReset(int channel)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    recalc_fc(a, PHROT_FC_DEFAULT);
    a->auto_fc           = a->fc;
    a->auto_asym_best    = -1.0;
    a->auto_step         = PHROT_STEP_INIT;
    a->auto_direction    = -1;
    a->auto_reject_count = 0;
    a->auto_converged    = 0;
    a->ab_phase          = 0;
    a->fc_A              = a->fc;
    a->fc_B              = a->fc;
    a->asym_A            = 0.0;
    a->optim_samp_count  = 0;
    LeaveCriticalSection(&a->cs_update);
}

PORT
void GetTXAPHROTAsymmetry(int channel,
                           double* in_pos,
                           double* in_neg,
                           double* in_ratio,
                           double* out_pos,
                           double* out_neg,
                           double* out_ratio,
                           double* current_fc,
                           double* auto_step)
{
    PHROT a = txa[channel].phrot.p;
    EnterCriticalSection(&a->cs_update);
    *in_pos     = a->in_env_pos;
    *in_neg     = a->in_env_neg;
    *in_ratio   = a->in_asym_metric;
    *out_pos    = a->env_pos;
    *out_neg    = a->env_neg;
    *out_ratio  = a->asym_metric;
    *current_fc = a->fc;
    *auto_step  = a->autoMode
                  ? (a->auto_converged ? -1.0 : a->auto_step)
                  : 0.0;
    LeaveCriticalSection(&a->cs_update);
}
