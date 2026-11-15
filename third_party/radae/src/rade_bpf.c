/*---------------------------------------------------------------------------*\

  rade_bpf.c

  Complex bandpass filter for RADAE.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rade_bpf.h"
#include <string.h>
#include <assert.h>

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

void rade_bpf_init(rade_bpf *bpf, int ntap, float Fs_Hz, float bandwidth_Hz,
                   float centre_freq_Hz, int max_len) {
    assert(ntap <= RADE_BPF_NTAP);
    assert(ntap % 2 == 1);  /* ntap should be odd for symmetric filter */

    bpf->ntap = ntap;
    bpf->alpha = 2.0f * M_PI * centre_freq_Hz / Fs_Hz;
    bpf->max_len = max_len;

    /* Generate lowpass filter coefficients using sinc function
       Bandwidth B = bandwidth_Hz / Fs_Hz (normalized)
       h[n] = B * sinc(n * B) where n is centered at (ntap-1)/2 */
    float B = bandwidth_Hz / Fs_Hz;
    for (int i = 0; i < ntap; i++) {
        int n = i - (ntap - 1)/2;
        bpf->h[i] = B * rade_sinc(n * B);
    }

    /* Initialize state */
    rade_bpf_reset(bpf);

    /* Pre-compute phase increment */
    bpf->phase_inc = rade_cexp(-bpf->alpha);
}

void rade_bpf_reset(rade_bpf *bpf) {
    /* Clear filter memory */
    memset(bpf->mem, 0, sizeof(RADE_COMP) * bpf->ntap);

    /* Reset mixer phase */
    bpf->phase = rade_cone();
}

/*---------------------------------------------------------------------------*\
                              PROCESSING
\*---------------------------------------------------------------------------*/

void rade_bpf_process(rade_bpf *bpf, RADE_COMP *y, const RADE_COMP *x, int n) {
    assert(n <= bpf->max_len);

    int ntap = bpf->ntap;
    RADE_COMP phase = bpf->phase;
    RADE_COMP phase_inc = bpf->phase_inc;

    /* Process each sample */
    for (int i = 0; i < n; i++) {
        /* Mix down to baseband using incremental phase (avoids per-sample sinf/cosf) */
        phase = rade_cmul(phase, phase_inc);
        RADE_COMP x_bb = rade_cmul(x[i], phase);

        /* Shift filter memory and add new sample */
        for (int index = ntap - 1; index >= 1; index--)
        {
            bpf->mem[index] = bpf->mem[index - 1];
        }
        bpf->mem[0] = x_bb;

        /* FIR filter: y_bb = sum(h[k] * mem[k])
           Since h is real and symmetric, we can optimize but keep simple for now */
        RADE_COMP y_bb = rade_cdot_float(bpf->mem, bpf->h, ntap);
        
        /* Mix back up to centre frequency: y = y_bb * conj(phase) */
        RADE_COMP cconj_phase = rade_cconj(phase);
        y[i] = rade_cmul(y_bb, cconj_phase);
    }

    /* Save phase state for next call */
    float phase_mag = rade_cabs(phase);
    phase = rade_cscale(phase, 1.0/phase_mag);
    bpf->phase = phase;
}
