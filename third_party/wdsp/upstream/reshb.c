/*  reshb.c

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
#include "comm.h"

static double* dcheb_compute(int N, double atten_db)
{
    double* s_dcheb_buf = (double*)malloc0(N * sizeof(double));
    int    order = N - 1;
    double beta = pow(10.0, atten_db / 20.0);
    double x0 = cosh(acosh(beta) / (double)order);
    double* W = (double*)malloc0(N * sizeof(double));
    for (int k = 0; k < N; k++)
    {
        double x = x0 * cos((PI * k) / (double)N);
        if (x > 1.0)
            W[k] = cosh((double)order * acosh(x));
        else if (x < -1.0)
            W[k] = (order % 2 == 0 ? 1.0 : -1.0)
            * cosh((double)order * acosh(-x));
        else
            W[k] = cos((double)order * acos(x));
    }
    int shift = (N + 1) / 2;
    for (int n = 0; n < N; n++)
    {
        double sum = 0.0;
        for (int k = 0; k < N; k++)
            sum += W[k] * cos((2.0 * PI * k * (n + shift)) / (double)N);
        s_dcheb_buf[n] = sum / (double)N;
    }
    int    M = (N - 1) / 2;
    double peak = s_dcheb_buf[M];
    for (int n = 0; n < N; n++)
        s_dcheb_buf[n] /= peak;
    _aligned_free(W);
    return s_dcheb_buf;
}

static void impulse_halfband_dcheb(double* h, int N, double atten_db)
{
    assert(N % 2 == 1);

    double* window = NULL;
    window = dcheb_compute(N, atten_db);

    int    M = (N - 1) / 2;
    double omega_c = PI / 2.0;

    for (int i = 0; i < N; i++)
    {
        int k = i - M;
        if (k == 0)
            h[i] = 0.5;
        else if (abs(k) % 2 == 0)
            h[i] = 0.0;
        else
        {
            double sinc = sin(omega_c * (double)k) / (PI * (double)k);
            h[i] = sinc * window[i];
        }
    }
    _aligned_free(window);
}



static double bh7 (int n, int N) 
{
    // For ODD 'N'
    const double a0 = 0.27105140069342;
    const double a1 = 0.43329793923448;
    const double a2 = 0.21812299954311;
    const double a3 = 0.06592544638803;
    const double a4 = 0.01081174209837;
    const double a5 = 0.00077658482522;
    const double a6 = 0.00001388721735;
    double arg = (TWOPI * (double)n) / (double)(N - 1);
    double w = a0
        - a1 * cos(arg)
        + a2 * cos(2.0 * arg)
        - a3 * cos(3.0 * arg)
        + a4 * cos(4.0 * arg)
        - a5 * cos(5.0 * arg)
        + a6 * cos(6.0 * arg);
    return w;
}

static void impulse_halfband (double* h, int N)
{
    assert (N % 2 == 1);
    int M = (N - 1) / 2;
    int i = 0, k = 0;
    double sinc = 0.0;
    double omega_c = PI / 2.0;
    for (i = 0; i < N; i++)
    {
        k = i - M;
        if (k == 0)           h[i] = 0.5;
        else if (abs(k) % 2 == 0)  h[i] = 0.0;
        else
        {
            sinc = sin (omega_c * (double)k) / (PI * (double)k);
            h[i] = sinc * bh7 (i, N);
        }
    }
}

static int xhbres(HBRES r)
{
    int n_out = 0;
    int center = (r->N - 1) / 2;
    int h_center_idx = 0, idx_left = 0, idx_right = 0;
    double hval = 0.0;
    complex_t accum = { 0.0, 0.0 };
    
    for (int i = 0; i < r->size; i++)
    {
        r->ring[r->ring_ptr] = r->in[i];
        if (i % 2 == 1)
        {
            accum.i = 0.0; accum.q = 0.0;
            h_center_idx = (r->ring_ptr - center + r->N) % r->N;
            accum.i = r->ring[h_center_idx].i * r->h[center];
            accum.q = r->ring[h_center_idx].q * r->h[center];
            for (int j = 1; j <= center; j += 2)
            {
                idx_left  = ((r->ring_ptr - (center - j)) % r->N + r->N) % r->N;
                idx_right = ((r->ring_ptr - (center + j)) % r->N + r->N) % r->N;
                hval = r->h[center - j];
                accum.i += (r->ring[idx_left].i + r->ring[idx_right].i) * hval;
                accum.q += (r->ring[idx_left].q + r->ring[idx_right].q) * hval;
            }
            r->out[n_out++] = accum;
        }
        if (++r->ring_ptr >= r->N) r->ring_ptr = 0;
    }
    return n_out;
}

void xHBResampler(HBResampler tData)
{
    if (tData->run)
    {
        for (uint32_t i = 0; i < tData->nStages; i++)
        {
            xhbres(&(tData->rsmps[i]));
        }
    }
    else
        memcpy(tData->out, tData->in, tData->insize * sizeof(complex_t));
}

static void calc_HBResampler(HBResampler tData)
{
    uint64_t rates = ((uint64_t)tData->inrate << 32 | (uint64_t)tData->outrate);
    switch (rates)
    {
    case ((uint64_t)6144000 << 32 | (uint64_t)192000):
        tData->run = 1;
        tData->nStages = 5;
        tData->taps[0] = 19;
        tData->taps[1] = 23;
        tData->taps[2] = 23;
        tData->taps[3] = 35;
        tData->taps[4] = 319;
        tData->nBuffs  = 4;
        break;
    case ((uint64_t)6144000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 6;
        tData->taps[0] = 19;
        tData->taps[1] = 23;
        tData->taps[2] = 23;
        tData->taps[3] = 35;
        tData->taps[4] = 35;
        tData->taps[5] = 319;
        tData->nBuffs  = 5;
        break;
    case ((uint64_t)6144000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 7;
        tData->taps[0] = 19;
        tData->taps[1] = 23;
        tData->taps[2] = 23;
        tData->taps[3] = 35;
        tData->taps[4] = 35;
        tData->taps[5] = 35;
        tData->taps[6] = 319;
        tData->nBuffs  = 6;
        break;
    case ((uint64_t)3072000 << 32 | (uint64_t)192000):
        tData->run = 1;
        tData->nStages = 4;
        tData->taps[0] = 23;
        tData->taps[1] = 23;
        tData->taps[2] = 35;
        tData->taps[3] = 319;
        tData->nBuffs  = 3;
        break;
    case ((uint64_t)3072000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 5;
        tData->taps[0] = 23;
        tData->taps[1] = 23;
        tData->taps[2] = 35;
        tData->taps[3] = 35;
        tData->taps[4] = 319;
        tData->nBuffs  = 4;
        break;
    case ((uint64_t)3072000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 6;
        tData->taps[0] = 23;
        tData->taps[1] = 23;
        tData->taps[2] = 35;
        tData->taps[3] = 35;
        tData->taps[4] = 35;
        tData->taps[5] = 319;
        tData->nBuffs  = 5;
        break;
    case ((uint64_t)1536000 << 32 | (uint64_t)192000):
        tData->run = 1;
        tData->nStages = 3;
        tData->taps[0] = 27;
        tData->taps[1] = 35;
        tData->taps[2] = 319;
        tData->nBuffs = 2;
        break;
    case ((uint64_t)1536000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 4;
        tData->taps[0] = 23;
        tData->taps[1] = 35;
        tData->taps[2] = 35;
        tData->taps[3] = 319;
        tData->nBuffs = 3;
        break;
    case ((uint64_t)1536000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 5;
        tData->taps[0] = 23;
        tData->taps[1] = 35;
        tData->taps[2] = 35;
        tData->taps[3] = 35;
        tData->taps[4] = 319;
        tData->nBuffs = 4;
        break;
    case ((uint64_t)768000 << 32 | (uint64_t)192000):
        tData->run = 1;
        tData->nStages = 2;
        tData->taps[0] = 35;
        tData->taps[1] = 319;
        tData->nBuffs = 1;
        break;
    case ((uint64_t)768000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 3;
        tData->taps[0] = 35;
        tData->taps[1] = 35;
        tData->taps[2] = 319;
        tData->nBuffs = 2;
        break;
    case ((uint64_t)768000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 4;
        tData->taps[0] = 35;
        tData->taps[1] = 35;
        tData->taps[2] = 35;
        tData->taps[3] = 319;
        tData->nBuffs = 3;
        break;
    case ((uint64_t)384000 << 32 | (uint64_t)192000):
        tData->run = 1;
        tData->nStages = 1;
        tData->taps[0] = 319;
        tData->nBuffs = 0;
        break;
    case ((uint64_t)384000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 2;
        tData->taps[0] = 35;
        tData->taps[1] = 319;
        tData->nBuffs = 1;
        break;
    case ((uint64_t)384000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 3;
        tData->taps[0] = 35;
        tData->taps[1] = 35;
        tData->taps[2] = 319;
        tData->nBuffs = 2;
        break;
    case ((uint64_t)192000 << 32 | (uint64_t)96000):
        tData->run = 1;
        tData->nStages = 1;
        tData->taps[0] = 319;
        tData->nBuffs = 0;
        break;
    case ((uint64_t)192000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 2;
        tData->taps[0] = 35;
        tData->taps[1] = 319;
        tData->nBuffs = 1;
        break;
    case ((uint64_t)96000 << 32 | (uint64_t)48000):
        tData->run = 1;
        tData->nStages = 1;
        tData->taps[0] = 319;
        tData->nBuffs = 0;
        break;
    default:
        tData->run = 0;
        tData->nStages = 0;
        tData->taps[0] = 3;
        tData->nBuffs = 0;
        break;
    }
    const int WINTYPE = 2;
    uint32_t bdiv = 2;
    for (uint32_t i = 0; i < tData->nBuffs; i++)
    {
        tData->bsize[i] = tData->insize / bdiv;
        bdiv *= 2;
    }
    for (uint32_t i = 0; i < tData->nBuffs; i++)
    {
        tData->buff[i] = (complex_t*)malloc0(tData->bsize[i] * sizeof(complex_t));
    }
    for (uint32_t i = 0; i < tData->nStages; i++)
    {
        if (i == 0)
        {
            tData->rsmps[i].in = tData->in;
            tData->rsmps[i].size = tData->insize;
        }
        else
        {
            tData->rsmps[i].in = tData->buff[i - 1];
            tData->rsmps[i].size = tData->bsize[i - 1];
        }
        if (i == tData->nStages - 1)
            tData->rsmps[i].out = tData->out;
        else 
            tData->rsmps[i].out = tData->buff[i];
        tData->rsmps[i].N = tData->taps[i];
        tData->rsmps[i].ring_ptr = 0;
        tData->rsmps[i].ring = (complex_t *)malloc0(tData->rsmps[i].N * sizeof(complex_t));
        tData->rsmps[i].h = (double*)malloc0(tData->rsmps[i].N * sizeof(double));
        if (WINTYPE == 1)
            impulse_halfband(tData->rsmps[i].h, tData->rsmps[i].N);
        else if (WINTYPE == 2)
        {
            double cheb_atten = 140.0;
            impulse_halfband_dcheb(tData->rsmps[i].h, tData->rsmps[i].N, cheb_atten);
        }
    }
}

HBResampler create_HBResampler(uint32_t inrate, uint32_t outrate,
    uint32_t insize, complex_t* in, complex_t* out)
{
    HBResampler tData = (HBResampler)malloc0(sizeof(struct HBRESdata));
    if (!tData) return NULL;
    tData->inrate = inrate;
    tData->outrate = outrate;
    assert(insize % 2 == 0);
    tData->insize = insize;
    tData->in = in;
    tData->out = out;
    calc_HBResampler(tData);
    return tData;
}

static void decalc_HBResampler(HBResampler tData)
{
    for (uint32_t i = 0; i < tData->nStages; i++)
    {
        _aligned_free(tData->rsmps[i].h);
        _aligned_free(tData->rsmps[i].ring);
    }
    for (uint32_t i = 0; i < tData->nBuffs; i++)
    {
        _aligned_free(tData->buff[i]);
    }
}

void destroy_HBResampler(HBResampler tData)
{
    decalc_HBResampler(tData);
    _aligned_free(tData);
}

void flush_HBResampler(HBResampler tData)
{
    for (uint32_t i = 0; i < tData->nStages; i++)
    {
        memset(tData->rsmps[i].ring, 0, tData->rsmps[i].N * sizeof(complex_t));
        tData->rsmps[i].ring_ptr = 0;
    }
    for (uint32_t i = 0; i < tData->nBuffs; i++)
    {
        memset(tData->buff[i], 0, tData->bsize[i] * sizeof(complex_t));
    }
}

void setBuffers_HBResampler(HBResampler tData, complex_t* in, complex_t* out)
{
    decalc_HBResampler(tData);
    tData->in = in;
    tData->out = out;
    calc_HBResampler(tData);
}

void setSize_HBResampler(HBResampler tData, uint32_t insize)
{
    decalc_HBResampler(tData);
    tData->insize = insize;
    calc_HBResampler(tData);
}

void setInRate_HBResampler(HBResampler tData, int inrate)
{
    decalc_HBResampler(tData);
    tData->inrate = inrate;
    calc_HBResampler(tData);
}

void setOutRate_HBResampler(HBResampler tData, int outrate)
{
    decalc_HBResampler(tData);
    tData->outrate = outrate;
    calc_HBResampler(tData);
}

