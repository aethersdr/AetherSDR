/*---------------------------------------------------------------------------*\

  rade_rx.h

  RADAE receiver - demodulates IQ samples to features.
  Includes acquisition, synchronization, and tracking.

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

#ifndef __RADE_RX__
#define __RADE_RX__

#include "rade_dsp.h"
#include "rade_ofdm.h"
#include "rade_bpf.h"
#include "rade_acq.h"
#include "rade_dec.h"
#include "rade_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                           RECEIVER STATE
\*---------------------------------------------------------------------------*/

/* Receive buffer size: 2*Nmf + M + Ncp */
#define RADE_RX_BUF_SIZE (2 * RADE_NMF + RADE_M + RADE_NCP)

typedef struct {
    /* DSP components */
    rade_ofdm ofdm;
    rade_bpf bpf;
    rade_acq acq;
    int bpf_en;

    /* Core decoder */
    RADEDec dec_model;
    RADEDecState dec_state;

    /* Configuration */
    int bottleneck;
    int auxdata;
    int num_features;
    int coarse_mag;
    int time_offset;          /* Fine timing adjustment (default -16) */

    /* State machine */
    int state;                /* RADE_STATE_SEARCH, CANDIDATE, or SYNC */
    int valid_count;
    int synced_count;
    int uw_errors;
    int Nmf_unsync;           /* Modem frames before losing sync */
    int synced_count_one_sec; /* Modem frames in one second */

    /* Timing and frequency tracking */
    int tmax;
    int tmax_candidate;
    float fmax;
    RADE_COMP rx_phase;
    int nin;                  /* Samples needed for next call */

    /* Receive buffer */
    RADE_COMP rx_buf[RADE_RX_BUF_SIZE];

    /* SNR estimate */
    float snrdB_3k_est;

    /* Frame counter */
    int mf;

    /* Verbosity */
    int verbose;

    /* Test mode: disable unsync after this many seconds (0 = disabled) */
    float disable_unsync;

} rade_rx_state;

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Initialize receiver
   dec_model: pointer to decoder model weights (can be NULL to use built-in)
   bottleneck: 1, 2, or 3
   auxdata: 1 to enable auxiliary data decoding
   bpf_en: 1 to enable input bandpass filter
   Returns 0 on success */
int rade_rx_init(rade_rx_state *rx, const RADEDec *dec_model, int bottleneck, int auxdata, int bpf_en);

/* Reset receiver state (go back to search mode) */
void rade_rx_reset(rade_rx_state *rx);

/*---------------------------------------------------------------------------*\
                           RECEPTION
\*---------------------------------------------------------------------------*/

/* Get number of samples needed for next call to rade_rx_process */
int rade_rx_nin(const rade_rx_state *rx);

/* Get maximum number of samples ever needed (for buffer allocation) */
int rade_rx_nin_max(const rade_rx_state *rx);

/* Get number of output features per valid frame */
int rade_rx_n_features_out(const rade_rx_state *rx);

/* Get number of EOO bits */
int rade_rx_n_eoo_bits(const rade_rx_state *rx);

/* Check if receiver is synchronized */
int rade_rx_sync(const rade_rx_state *rx);

/* Get estimated SNR in dB (3kHz noise bandwidth) */
float rade_rx_snrdB_3k_est(const rade_rx_state *rx);

/* Get current frequency offset estimate */
float rade_rx_freq_offset(const rade_rx_state *rx);

/* Process received samples
   rx_in: input IQ samples [nin]
   features_out: output features [n_features_out] (valid only if return & 1)
   eoo_out: output EOO bits [n_eoo_bits] (valid only if return & 2)

   Returns:
   - bit 0 (0x1): valid speech features output
   - bit 1 (0x2): end-of-over detected, eoo_out contains soft decision bits */
int rade_rx_process(rade_rx_state *rx, float *features_out, float *eoo_out, const RADE_COMP *rx_in);

/* Report unique word errors (called externally if C decoder is used)
   This is used by the state machine for unsync detection */
void rade_rx_sum_uw_errors(rade_rx_state *rx, int new_uw_errors);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_RX__ */
