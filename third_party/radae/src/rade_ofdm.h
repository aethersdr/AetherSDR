/*---------------------------------------------------------------------------*\

  rade_ofdm.h

  OFDM modulation and demodulation for RADAE.
  Handles DFT/IDFT, pilot insertion, cyclic prefix, and equalization.

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

#ifndef __RADE_OFDM__
#define __RADE_OFDM__

#include "rade_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                              OFDM STATE
\*---------------------------------------------------------------------------*/

typedef struct {
    /* Configuration */
    int nc;                                     /* Number of carriers */
    int m;                                      /* Samples per OFDM symbol */
    int ncp;                                    /* Cyclic prefix samples */
    int ns;                                     /* Data symbols per modem frame */
    int bottleneck;                             /* Bottleneck mode (1, 2, or 3) */

    /* DFT matrices - pre-computed at init */
    RADE_COMP Winv[RADE_M][RADE_NC];           /* IDFT matrix (Tx): Nc freq -> M time */
    RADE_COMP Wfwd[RADE_NC][RADE_M];           /* DFT matrix (Rx): M time -> Nc freq */

    /* Carrier frequencies */
    float w[RADE_NC];                           /* Angular frequency per carrier */

    /* Pilot symbols */
    RADE_COMP P[RADE_NC];                       /* Normal pilot symbols (Barker) */
    RADE_COMP Pend[RADE_NC];                    /* End-of-over pilot symbols */
    RADE_COMP p[RADE_M];                        /* Time-domain pilot (no CP) */
    RADE_COMP pend[RADE_M];                     /* Time-domain EOO pilot (no CP) */
    RADE_COMP p_cp[RADE_M + RADE_NCP];          /* Time-domain pilot with CP */
    RADE_COMP pend_cp[RADE_M + RADE_NCP];       /* Time-domain EOO pilot with CP */
    float pilot_gain;                           /* Pilot amplitude scaling */

    /* Pre-computed EOO frame */
    RADE_COMP eoo[RADE_NEOO];                   /* Complete EOO frame */
    int n_eoo;                                  /* EOO frame length */

    /* Equalization matrices - pre-computed at init */
    /* For 3-pilot least-squares fit: Pmat[c] = (A^H A)^-1 A^H */
    RADE_COMP Pmat[RADE_NC][2][3];              /* Per-carrier EQ matrices */
    float local_path_delay_s;                   /* Assumed path delay for LS EQ */

} rade_ofdm;

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Initialize OFDM state with default parameters
   - Computes DFT matrices
   - Generates pilot symbols
   - Pre-computes EOO frame
   - Pre-computes equalization matrices */
void rade_ofdm_init(rade_ofdm *ofdm, int bottleneck);

/*---------------------------------------------------------------------------*\
                           MODULATION (TX)
\*---------------------------------------------------------------------------*/

/* IDFT: Transform Nc frequency-domain carriers to M time-domain samples
   freq_in[nc], time_out[m] */
void rade_ofdm_idft(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *freq_in);

/* Insert cyclic prefix: M samples -> M+Ncp samples
   time_in[m], time_out[m+ncp] */
void rade_ofdm_insert_cp(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *time_in);

/* Modulate one modem frame of latent vectors to time-domain samples
   z[nzmf][latent_dim] -> tx_out[nmf]
   Returns number of output samples */
int rade_ofdm_mod_frame(const rade_ofdm *ofdm, RADE_COMP *tx_out, const float *z);

/*---------------------------------------------------------------------------*\
                          DEMODULATION (RX)
\*---------------------------------------------------------------------------*/

/* DFT: Transform M time-domain samples to Nc frequency-domain carriers
   time_in[m], freq_out[nc] */
void rade_ofdm_dft(const rade_ofdm *ofdm, RADE_COMP *freq_out, const RADE_COMP *time_in);

/* Remove cyclic prefix: M+Ncp samples -> M samples
   time_in[m+ncp], time_out[m], time_offset for fine timing adjustment */
void rade_ofdm_remove_cp(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *time_in, int time_offset);

/* Estimate pilot channel response using 3-pilot least-squares fit
   rx_pilots[num_pilots][nc] -> pilot_est[num_pilots][nc]
   num_pilots is typically 2 (start and end of modem frame) */
void rade_ofdm_est_pilots(const rade_ofdm *ofdm, RADE_COMP *pilot_est,
                          const RADE_COMP *rx_pilots, int num_pilots);

/* Equalize data symbols using interpolated pilot estimates
   rx_sym[ns][nc], pilot_est[2][nc] -> rx_sym_eq[ns][nc] (in-place)
   Returns estimated SNR in dB (3kHz bandwidth) */
float rade_ofdm_pilot_eq(const rade_ofdm *ofdm, RADE_COMP *rx_sym,
                         const RADE_COMP *rx_pilots_start,
                         const RADE_COMP *pilot_est_start, const RADE_COMP *pilot_est_end,
                         int coarse_mag);

/* Demodulate one modem frame of time-domain samples to latent vectors
   rx_in[nmf+m+ncp] -> z_hat[nzmf*latent_dim]
   tmax: timing offset, endofover: flag for EOO processing
   Returns number of output floats (latent_dim * nzmf) */
int rade_ofdm_demod_frame(const rade_ofdm *ofdm, float *z_hat, const RADE_COMP *rx_in,
                          int time_offset, int endofover, int coarse_mag, float *snr_est);

/*---------------------------------------------------------------------------*\
                           EOO HANDLING
\*---------------------------------------------------------------------------*/

/* Get pre-computed EOO frame
   Returns pointer to EOO samples, sets *n_out to number of samples */
const RADE_COMP* rade_ofdm_get_eoo(const rade_ofdm *ofdm, int *n_out);

/* Demodulate EOO frame (simpler equalization)
   rx_in: received EOO frame samples
   z_hat: output demodulated symbols
   Returns number of output floats */
int rade_ofdm_demod_eoo(const rade_ofdm *ofdm, float *z_hat, const RADE_COMP *rx_in, int time_offset);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_OFDM__ */
