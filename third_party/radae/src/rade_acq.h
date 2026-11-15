/*---------------------------------------------------------------------------*\

  rade_acq.h

  Pilot-based acquisition and synchronization for RADAE.

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

#ifndef __RADE_ACQ__
#define __RADE_ACQ__

#include "rade_dsp.h"
#include "rade_ofdm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                           ACQUISITION STATE
\*---------------------------------------------------------------------------*/

typedef struct {
    /* Configuration */
    int fs;                                     /* Sample rate */
    int m;                                      /* Samples per OFDM symbol */
    int ncp;                                    /* Cyclic prefix samples */
    int nmf;                                    /* Samples per modem frame */

    /* Frequency search range */
    float fcoarse_range[RADE_ACQ_NFREQ];       /* Frequency offsets to search */
    int n_fcoarse;                              /* Number of frequency steps */

    /* Pre-computed frequency-shifted pilots: p_w[M][n_freq] */
    RADE_COMP p_w[RADE_ACQ_NFREQ][RADE_M];

    /* Pilot power for normalization */
    float sigma_p;

    /* Pilot reference (from OFDM) */
    RADE_COMP p[RADE_M];                        /* Time-domain pilot */
    RADE_COMP pend[RADE_M];                     /* EOO pilot */

    /* Correlation grid (for threshold calculation) */
    RADE_COMP Dt1[RADE_NMF][RADE_ACQ_NFREQ];   /* Correlation at first pilot */
    RADE_COMP Dt2[RADE_NMF][RADE_ACQ_NFREQ];   /* Correlation at second pilot */

    /* Detection thresholds and results */
    float Dthresh;
    float Dtmax12;
    float Dtmax12_eoo;
    int f_ind_max;

    /* Acquisition probabilities */
    float Pacq_error1;
    float Pacq_error2;

} rade_acq;

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Initialize acquisition state
   ofdm: pointer to OFDM state (for pilot symbols)
   frange: frequency search range in Hz (e.g., 100)
   fstep: frequency search step in Hz (e.g., 2.5) */
void rade_acq_init(rade_acq *acq, const rade_ofdm *ofdm, float frange, float fstep);

/*---------------------------------------------------------------------------*\
                           PILOT DETECTION
\*---------------------------------------------------------------------------*/

/* Detect pilots in received signal
   rx: received samples, length = 2*Nmf + M + Ncp
   tmax: output timing offset (samples from start of rx)
   fmax: output frequency offset (Hz)
   Returns 1 if candidate detected, 0 otherwise */
int rade_acq_detect_pilots(rade_acq *acq, const RADE_COMP *rx, int *tmax, float *fmax);

/* Refine timing and frequency estimates
   rx: received samples
   tmax: input/output timing estimate
   fmax: input/output frequency estimate
   tfine_range_start, tfine_range_end: timing search range
   ffine_range_start, ffine_range_end, ffine_step: frequency search range */
void rade_acq_refine(rade_acq *acq, const RADE_COMP *rx,
                     int *tmax, float *fmax,
                     int tfine_range_start, int tfine_range_end,
                     float ffine_range_start, float ffine_range_end, float ffine_step);

/* Check pilots at current timing/frequency (for sync maintenance)
   rx: received samples, length = 2*Nmf + M + Ncp
   tmax: timing offset
   fmax: frequency offset
   valid: output 1 if pilots still valid
   endofover: output 1 if EOO detected
   Returns 1 if still synced, 0 otherwise */
int rade_acq_check_pilots(rade_acq *acq, const RADE_COMP *rx,
                          int tmax, float fmax,
                          int *valid, int *endofover);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_ACQ__ */
