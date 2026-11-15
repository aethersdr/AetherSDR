/*---------------------------------------------------------------------------*\

  rade_bpf.h

  Complex bandpass filter for RADAE.
  Mix down to baseband, filter, mix back up.

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

#ifndef __RADE_BPF__
#define __RADE_BPF__

#include "rade_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                              BPF STATE
\*---------------------------------------------------------------------------*/

typedef struct {
    int ntap;                               /* Number of filter taps */
    float alpha;                            /* 2*pi*centre_freq/Fs (rad/sample) */
    float h[RADE_BPF_NTAP];                /* Filter coefficients (real, symmetric) */
    RADE_COMP mem[RADE_BPF_NTAP];          /* Filter memory/state */
    RADE_COMP phase;                        /* Mixer phase state */
    RADE_COMP phase_inc;                    /* Phase increment per sample */
    int max_len;                            /* Maximum input length */
} rade_bpf;

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Initialize BPF with specified parameters
   ntap: number of filter taps (should be odd, typically 101)
   Fs_Hz: sample rate in Hz
   bandwidth_Hz: filter bandwidth in Hz
   centre_freq_Hz: centre frequency in Hz
   max_len: maximum input block length */
void rade_bpf_init(rade_bpf *bpf, int ntap, float Fs_Hz, float bandwidth_Hz,
                   float centre_freq_Hz, int max_len);

/* Reset BPF state (clear memory and phase) */
void rade_bpf_reset(rade_bpf *bpf);

/*---------------------------------------------------------------------------*\
                              PROCESSING
\*---------------------------------------------------------------------------*/

/* Process samples through the BPF
   x: input samples (complex)
   y: output samples (complex)
   n: number of samples to process (must be <= max_len)

   The filter:
   1. Mixes input down to baseband
   2. Applies lowpass FIR filter
   3. Mixes result back up to centre frequency

   This effectively creates a bandpass filter centered at centre_freq_Hz
   with bandwidth bandwidth_Hz. The negative frequency image is suppressed. */
void rade_bpf_process(rade_bpf *bpf, RADE_COMP *y, const RADE_COMP *x, int n);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_BPF__ */
