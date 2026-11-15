/*---------------------------------------------------------------------------*\

  rade_ofdm.c

  OFDM modulation and demodulation for RADAE.

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

#include "rade_ofdm.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

void rade_ofdm_init(rade_ofdm *ofdm, int bottleneck) {
    int Nc = RADE_NC;
    int M = RADE_M;
    int Ncp = RADE_NCP;
    int Ns = RADE_NS;
    float Fs = (float)RADE_FS;

    ofdm->nc = Nc;
    ofdm->m = M;
    ofdm->ncp = Ncp;
    ofdm->ns = Ns;
    ofdm->bottleneck = bottleneck;
    ofdm->local_path_delay_s = 0.0025f;  /* 2.5ms assumed path delay */

    /* Calculate carrier frequencies
       Centre signal on 1500 Hz (middle of SSB passband)
       Rs' = Fs/M is the symbol rate with pilots and CP */
    float Rs_dash = Fs / M;
    float carrier_1_freq = 1500.0f - Rs_dash * Nc / 2.0f;
    int carrier_1_index = (int)roundf(carrier_1_freq / Rs_dash);

    for (int c = 0; c < Nc; c++) {
        ofdm->w[c] = 2.0f * M_PI * (carrier_1_index + c) / M;
    }

    /* Compute IDFT matrix (Tx): Winv[c][n] = exp(j*w[c]*n) / M
       freq_in[Nc] * Winv[Nc][M] -> time_out[M] */
    for (int c = 0; c < Nc; c++) {
        for (int n = 0; n < M; n++) {
            float theta = ofdm->w[c] * n;
            ofdm->Winv[n][c] = rade_cscale(rade_cexp(theta), 1.0f / M);
        }
    }

    /* Compute DFT matrix (Rx): Wfwd[n][c] = exp(-j*w[c]*n)
       time_in[M] * Wfwd[M][Nc] -> freq_out[Nc] */
    for (int n = 0; n < M; n++) {
        for (int c = 0; c < Nc; c++) {
            float theta = -ofdm->w[c] * n;
            ofdm->Wfwd[c][n] = rade_cexp(theta);
        }
    }

    /* Generate pilot symbols */
    rade_barker_pilots(ofdm->P, Nc);
    rade_eoo_pilots(ofdm->Pend, ofdm->P, Nc);

    /* Compute pilot gain for bottleneck 3 (PA saturation) */
    if (bottleneck == 3) {
        float pilot_backoff = powf(10.0f, -2.0f / 20.0f);  /* -2 dB backoff */
        ofdm->pilot_gain = pilot_backoff * M / sqrtf((float)Nc);
    } else {
        ofdm->pilot_gain = 1.0f;
    }

    /* Compute time-domain pilots: p = P * Winv^T */
    memset(ofdm->p, 0, sizeof(ofdm->p));
    memset(ofdm->pend, 0, sizeof(ofdm->pend));
    for (int n = 0; n < M; n++) {
        for (int c = 0; c < Nc; c++) {
            ofdm->p[n] = rade_cadd(ofdm->p[n], rade_cmul(ofdm->P[c], ofdm->Winv[n][c]));
            ofdm->pend[n] = rade_cadd(ofdm->pend[n], rade_cmul(ofdm->Pend[c], ofdm->Winv[n][c]));
        }
    }

    /* Compute time-domain pilots with cyclic prefix */
    if (Ncp > 0) {
        /* Copy pilot to p_cp with CP at front */
        for (int n = 0; n < M; n++) {
            ofdm->p_cp[Ncp + n] = ofdm->p[n];
            ofdm->pend_cp[Ncp + n] = ofdm->pend[n];
        }
        /* Cyclic prefix is last Ncp samples copied to front */
        for (int n = 0; n < Ncp; n++) {
            ofdm->p_cp[n] = ofdm->p[M - Ncp + n];
            ofdm->pend_cp[n] = ofdm->pend[M - Ncp + n];
        }
    }

    /* Pre-compute EOO frame:
       Normal frame: ...PDDDDP...
       EOO frame:    ...PE000E... (P=pilot, E=EOO pilot, D=data, 0=zeros)
       Frame structure: [p_cp][pend_cp][zeros...][pend_cp] */
    int Nmf = (Ns + 1) * (M + Ncp);
    memset(ofdm->eoo, 0, sizeof(ofdm->eoo));

    /* First pilot symbol */
    for (int n = 0; n < M + Ncp; n++) {
        ofdm->eoo[n] = rade_cscale(ofdm->p_cp[n], ofdm->pilot_gain);
    }
    /* Second symbol is EOO pilot */
    for (int n = 0; n < M + Ncp; n++) {
        ofdm->eoo[M + Ncp + n] = rade_cscale(ofdm->pend_cp[n], ofdm->pilot_gain);
    }
    /* Last symbol is EOO pilot */
    for (int n = 0; n < M + Ncp; n++) {
        ofdm->eoo[Nmf + n] = rade_cscale(ofdm->pend_cp[n], ofdm->pilot_gain);
    }

    /* Apply PA saturation to EOO frame if bottleneck == 3 */
    if (bottleneck == 3) {
        for (int n = 0; n < RADE_NEOO; n++) {
            ofdm->eoo[n] = rade_tanh_limit(ofdm->eoo[n]);
        }
    }
    ofdm->n_eoo = Nmf + M + Ncp;

    /* Pre-compute equalization matrices for 3-pilot LS fit
       For each carrier c, we fit: h = g0 + g1*exp(-j*w[c]*a)
       where a = local_path_delay_s * Fs
       Using pilots at c-1, c, c+1 (edge carriers use adjusted indices) */
    float a = ofdm->local_path_delay_s * Fs;

    for (int c = 0; c < Nc; c++) {
        int c_mid = c;
        /* Handle edge carriers */
        if (c == 0) c_mid = 1;
        if (c == Nc - 1) c_mid = Nc - 2;

        /* Build 3x2 matrix A for LS fit */
        /* A = [[1, exp(-j*w[c_mid-1]*a)],
               [1, exp(-j*w[c_mid]*a)],
               [1, exp(-j*w[c_mid+1]*a)]] */
        RADE_COMP A[3][2];
        for (int i = 0; i < 3; i++) {
            A[i][0] = rade_cone();
            A[i][1] = rade_cexp(-ofdm->w[c_mid - 1 + i] * a);
        }

        /* Compute A^H * A (2x2 Hermitian matrix) */
        RADE_COMP AHA[2][2];
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                AHA[i][j] = rade_czero();
                for (int k = 0; k < 3; k++) {
                    /* AHA[i][j] += conj(A[k][i]) * A[k][j] */
                    AHA[i][j] = rade_cadd(AHA[i][j], rade_cmul(rade_cconj(A[k][i]), A[k][j]));
                }
            }
        }

        /* Compute (A^H * A)^-1 (2x2 inverse) */
        RADE_COMP det = rade_csub(rade_cmul(AHA[0][0], AHA[1][1]), rade_cmul(AHA[0][1], AHA[1][0]));
        RADE_COMP AHAinv[2][2];
        AHAinv[0][0] = rade_cdiv(AHA[1][1], det);
        AHAinv[0][1] = rade_cdiv(rade_cscale(AHA[0][1], -1.0f), det);
        AHAinv[1][0] = rade_cdiv(rade_cscale(AHA[1][0], -1.0f), det);
        AHAinv[1][1] = rade_cdiv(AHA[0][0], det);

        /* Compute Pmat = (A^H * A)^-1 * A^H (2x3 matrix) */
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                ofdm->Pmat[c][i][j] = rade_czero();
                for (int k = 0; k < 2; k++) {
                    /* Pmat[i][j] += AHAinv[i][k] * conj(A[j][k]) */
                    ofdm->Pmat[c][i][j] = rade_cadd(ofdm->Pmat[c][i][j],
                        rade_cmul(AHAinv[i][k], rade_cconj(A[j][k])));
                }
            }
        }
    }
}

/*---------------------------------------------------------------------------*\
                           MODULATION (TX)
\*---------------------------------------------------------------------------*/

/* IDFT: freq_in[Nc] -> time_out[M] */
void rade_ofdm_idft(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *freq_in) {
    int M = ofdm->m;
    int Nc = ofdm->nc;

    for (int n = 0; n < M; n++) {
        time_out[n] = rade_cdot_comp(freq_in, ofdm->Winv[n], Nc);
    }
}

/* Insert cyclic prefix */
void rade_ofdm_insert_cp(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *time_in) {
    int M = ofdm->m;
    int Ncp = ofdm->ncp;

    /* Cyclic prefix: copy last Ncp samples to front */
    memcpy(time_out, &time_in[M - Ncp], sizeof(RADE_COMP) * Ncp);
    //for (int n = 0; n < Ncp; n++) {
    //    time_out[n] = time_in[M - Ncp + n];
    //}
    /* Copy main symbol */
    memcpy(&time_out[Ncp], time_in, sizeof(RADE_COMP) * M);
    //for (int n = 0; n < M; n++) {
    //    time_out[Ncp + n] = time_in[n];
    //}
}

/* Modulate one modem frame */
int rade_ofdm_mod_frame(const rade_ofdm *ofdm, RADE_COMP *tx_out, const float *z) {
    int Nc = ofdm->nc;
    int M = ofdm->m;
    int Ncp = ofdm->ncp;
    int Ns = ofdm->ns;
    int latent_dim = RADE_LATENT_DIM;
    int Nzmf = RADE_NZMF;

    /* Total output samples per modem frame */
    int Nmf = (Ns + 1) * (M + Ncp);
    int out_idx = 0;

    /* Map latent vectors to QPSK symbols
       z is [Nzmf][latent_dim], we need [Ns][Nc] QPSK symbols
       Each latent vector maps to latent_dim/2 complex symbols */
    RADE_COMP tx_sym[RADE_NS][RADE_NC];

    /* z layout: z[vec][dim] where dim alternates real/imag
       Total symbols = Nzmf * latent_dim / 2 = 3 * 80 / 2 = 120
       Symbols per OFDM symbol = Nc = 30
       So Ns = 120 / 30 = 4 */
    int sym_idx = 0;
    for (int s = 0; s < Ns; s++) {
        for (int c = 0; c < Nc; c++) {
            int z_idx = sym_idx * 2;  /* Index into flattened z array */
            tx_sym[s][c].real = z[z_idx];
            tx_sym[s][c].imag = z[z_idx + 1];

            /* Apply magnitude constraint for bottleneck 2 */
            if (ofdm->bottleneck == 2) {
                tx_sym[s][c] = rade_tanh_limit(tx_sym[s][c]);
            }
            sym_idx++;
        }
    }

    /* Insert pilot at start of modem frame */
    RADE_COMP pilot_sym[RADE_NC];
    for (int c = 0; c < Nc; c++) {
        pilot_sym[c] = rade_cscale(ofdm->P[c], ofdm->pilot_gain);
    }

    /* Modulate pilot symbol */
    RADE_COMP time_buf[RADE_M];
    RADE_COMP time_cp[RADE_M + RADE_NCP];

    rade_ofdm_idft(ofdm, time_buf, pilot_sym);
    rade_ofdm_insert_cp(ofdm, time_cp, time_buf);

    /* Apply PA saturation for bottleneck 3 */
    if (ofdm->bottleneck == 3) {
        for (int n = 0; n < M + Ncp; n++) {
            time_cp[n] = rade_tanh_limit(time_cp[n]);
        }
    }

    /* Copy pilot to output */
    for (int n = 0; n < M + Ncp; n++) {
        tx_out[out_idx++] = time_cp[n];
    }

    /* Modulate data symbols */
    for (int s = 0; s < Ns; s++) {
        rade_ofdm_idft(ofdm, time_buf, tx_sym[s]);
        rade_ofdm_insert_cp(ofdm, time_cp, time_buf);

        /* Apply PA saturation for bottleneck 3 */
        if (ofdm->bottleneck == 3) {
            for (int n = 0; n < M + Ncp; n++) {
                time_cp[n] = rade_tanh_limit(time_cp[n]);
            }
        }

        for (int n = 0; n < M + Ncp; n++) {
            tx_out[out_idx++] = time_cp[n];
        }
    }

    assert(out_idx == Nmf);
    return Nmf;
}

/*---------------------------------------------------------------------------*\
                          DEMODULATION (RX)
\*---------------------------------------------------------------------------*/

/* DFT: time_in[M] -> freq_out[Nc] */
void rade_ofdm_dft(const rade_ofdm *ofdm, RADE_COMP *freq_out, const RADE_COMP *time_in) {
    int M = ofdm->m;
    int Nc = ofdm->nc;

    for (int c = 0; c < Nc; c++) {
        freq_out[c] = rade_cdot_comp(time_in, ofdm->Wfwd[c], M);
    }
}

/* Remove cyclic prefix with time offset adjustment */
void rade_ofdm_remove_cp(const rade_ofdm *ofdm, RADE_COMP *time_out, const RADE_COMP *time_in, int time_offset) {
    int M = ofdm->m;
    int Ncp = ofdm->ncp;

    /* Skip CP and apply time offset */
    memcpy(time_out, &time_in[Ncp + time_offset], sizeof(RADE_COMP) * M);
    //for (int n = 0; n < M; n++) {
    //    time_out[n] = time_in[Ncp + time_offset + n];
    //}
}

/* Estimate pilots using 3-pilot LS fit */
void rade_ofdm_est_pilots(const rade_ofdm *ofdm, RADE_COMP *pilot_est,
                          const RADE_COMP *rx_pilots, int num_pilots) {
    int Nc = ofdm->nc;
    float Fs = (float)RADE_FS;
    float a = ofdm->local_path_delay_s * Fs;

    for (int p = 0; p < num_pilots; p++) {
        const RADE_COMP *rx_p = &rx_pilots[p * Nc];
        RADE_COMP *est_p = &pilot_est[p * Nc];

        for (int c = 0; c < Nc; c++) {
            int c_mid = c;
            if (c == 0) c_mid = 1;
            if (c == Nc - 1) c_mid = Nc - 2;

            /* h = rx_p / P (element-wise for 3 neighboring carriers) */
            RADE_COMP h[3];
            for (int i = 0; i < 3; i++) {
                h[i] = rade_cdiv(rx_p[c_mid - 1 + i], ofdm->P[c_mid - 1 + i]);
            }

            /* g = Pmat * h (2x3 * 3x1 = 2x1) */
            RADE_COMP g[2];
            for (int i = 0; i < 2; i++) {
                g[i] = rade_czero();
                for (int j = 0; j < 3; j++) {
                    g[i] = rade_cadd(g[i], rade_cmul(ofdm->Pmat[c][i][j], h[j]));
                }
            }

            /* Channel estimate at carrier c: h_c = g[0] + g[1]*exp(-j*w[c]*a) */
            est_p[c] = rade_cadd(g[0], rade_cmul(g[1], rade_cexp(-ofdm->w[c] * a)));
        }
    }
}

/* Equalize data symbols using pilot estimates */
float rade_ofdm_pilot_eq(const rade_ofdm *ofdm, RADE_COMP *rx_sym,
                         const RADE_COMP *rx_pilots_start,
                         const RADE_COMP *pilot_est_start, const RADE_COMP *pilot_est_end,
                         int coarse_mag) {
    int Nc = ofdm->nc;
    int Ns = ofdm->ns;
    int M = ofdm->m;
    int Ncp = ofdm->ncp;

    /* Compute SNR estimate from first pilot
       Matches Python: update_snr_est() in radae/dsp.py lines 438-444
       S1 = signal power from received pilot symbols
       S2 = noise power from phase-corrected received pilots */
    float S1 = 0.0f, S2 = 0.0f;
    for (int c = 0; c < Nc; c++) {
        /* S1: signal power from received pilot symbols (not channel estimate!) */
        float mag2 = rade_cabs2(rx_pilots_start[c]);
        S1 += mag2;

        /* S2: noise estimate from phase-corrected received pilots
           Use phase from channel estimate to correct received pilots */
        float rx_phase = rade_cangle(pilot_est_start[c]);
        RADE_COMP Rcn_hat = rade_cmul(rx_pilots_start[c], rade_cexp(-rx_phase));
        S2 += Rcn_hat.imag * Rcn_hat.imag;
    }
    S2 += 1e-12f;  /* Avoid division by zero */
    float snr_est = S1 / (2.0f * S2) - 1.0f;
    if (snr_est <= 0.0f) snr_est = 0.1f;
    float snrdB_est = 10.0f * log10f(snr_est);

    /* Correction based on average of straight line fit to AWGN/MPG/MPP */
    float m_corr = 0.7650f;
    float c_corr = 4.1343f;
    snrdB_est = (snrdB_est - c_corr) / m_corr;

    /* Convert to 3kHz noise bandwidth */
    float Rs = (float)RADE_FS / M;
    float snrdB_3k = snrdB_est + 10.0f * log10f(Rs * Nc / 3000.0f) +
                     10.0f * log10f((float)(M + Ncp) / M);

    /* Linearly interpolate channel estimate between pilots and equalize */
    for (int s = 0; s < Ns; s++) {
        /* Interpolation factor: pilot at 0, data at 1..Ns, pilot at Ns+1 */
        float t = (float)(s) / (float)(Ns + 1);

        for (int c = 0; c < Nc; c++) {
            /* Interpolated channel estimate */
            RADE_COMP ch_est = rade_clerp(pilot_est_start[c], pilot_est_end[c], t);

            /* Phase correction only */
            float ch_angle = rade_cangle(ch_est);
            rx_sym[s * Nc + c] = rade_cmul(rx_sym[s * Nc + c], rade_cexp(-ch_angle));
        }
    }

    /* Coarse magnitude correction */
    if (coarse_mag) {
        float mag_sum = 0.0f;
        for (int c = 0; c < Nc; c++) {
            mag_sum += rade_cabs2(pilot_est_start[c]) + rade_cabs2(pilot_est_end[c]);
        }
        float mag = sqrtf(mag_sum / (2.0f * Nc)) + 1e-6f;

        if (ofdm->bottleneck == 3) {
            mag = mag * rade_cabs(ofdm->P[0]) / ofdm->pilot_gain;
        }

        float inv_mag = 1.0f / mag;
        for (int s = 0; s < Ns; s++) {
            for (int c = 0; c < Nc; c++) {
                rx_sym[s * Nc + c] = rade_cscale(rx_sym[s * Nc + c], inv_mag);
            }
        }
    }

    return snrdB_3k;
}

/* Demodulate one modem frame */
int rade_ofdm_demod_frame(const rade_ofdm *ofdm, float *z_hat, const RADE_COMP *rx_in,
                          int time_offset, int endofover, int coarse_mag, float *snr_est) {
    int Nc = ofdm->nc;
    int M = ofdm->m;
    int Ncp = ofdm->ncp;
    int Ns = ofdm->ns;

    /* Buffer for received symbols: pilot + Ns data + pilot = Ns+2 symbols */
    RADE_COMP rx_sym[(RADE_NS + 2)][RADE_NC];
    RADE_COMP time_buf[RADE_M];

    /* Demodulate all symbols in frame */
    for (int s = 0; s < Ns + 2; s++) {
        int sample_offset = s * (M + Ncp);
        rade_ofdm_remove_cp(ofdm, time_buf, &rx_in[sample_offset], time_offset);
        rade_ofdm_dft(ofdm, rx_sym[s], time_buf);
    }

    if (!endofover) {
        /* Normal frame: estimate pilots and equalize */
        RADE_COMP pilot_est[2][RADE_NC];

        /* First pilot at symbol 0, second at symbol Ns+1 */
        RADE_COMP rx_pilots[2 * RADE_NC];
        memcpy(&rx_pilots[0], rx_sym[0], sizeof(RADE_COMP) * Nc);
        memcpy(&rx_pilots[Nc], rx_sym[Ns + 1], sizeof(RADE_COMP) * Nc);

        rade_ofdm_est_pilots(ofdm, (RADE_COMP*)pilot_est, rx_pilots, 2);

        /* Equalize data symbols (symbols 1 to Ns) */
        RADE_COMP rx_data[RADE_NS * RADE_NC];
        for (int s = 0; s < Ns; s++) {
            memcpy(&rx_data[s * Nc], rx_sym[s + 1], sizeof(RADE_COMP) * Nc);
        }

        *snr_est = rade_ofdm_pilot_eq(ofdm, rx_data, &rx_pilots[0], pilot_est[0], pilot_est[1], coarse_mag);

        /* Demap QPSK to latent floats */
        int out_idx = 0;
        for (int s = 0; s < Ns; s++) {
            for (int c = 0; c < Nc; c++) {
                z_hat[out_idx++] = rx_data[s * Nc + c].real;
                z_hat[out_idx++] = rx_data[s * Nc + c].imag;
            }
        }

        return out_idx;
    } else {
        /* EOO frame - use simpler equalization */
        return rade_ofdm_demod_eoo(ofdm, z_hat, rx_in, time_offset);
    }
}

/* Get EOO frame */
const RADE_COMP* rade_ofdm_get_eoo(const rade_ofdm *ofdm, int *n_out) {
    *n_out = ofdm->n_eoo;
    return ofdm->eoo;
}

/* Demodulate EOO frame */
int rade_ofdm_demod_eoo(const rade_ofdm *ofdm, float *z_hat, const RADE_COMP *rx_in, int time_offset) {
    int Nc = ofdm->nc;
    int M = ofdm->m;
    int Ncp = ofdm->ncp;
    int Ns = ofdm->ns;

    /* EOO frame structure: P E 0 0 0 E
       Demodulate all Ns+2 symbols */
    RADE_COMP rx_sym[(RADE_NS + 2)][RADE_NC];
    RADE_COMP time_buf[RADE_M];

    for (int s = 0; s < Ns + 2; s++) {
        int sample_offset = s * (M + Ncp);
        rade_ofdm_remove_cp(ofdm, time_buf, &rx_in[sample_offset], time_offset);
        rade_ofdm_dft(ofdm, rx_sym[s], time_buf);
    }

    /* Simpler EQ: average phase from P, E1, E2 pilots */
    for (int c = 0; c < Nc; c++) {
        RADE_COMP sum = rade_czero();
        sum = rade_cadd(sum, rade_cdiv(rx_sym[0][c], ofdm->P[c]));
        sum = rade_cadd(sum, rade_cdiv(rx_sym[1][c], ofdm->Pend[c]));
        sum = rade_cadd(sum, rade_cdiv(rx_sym[Ns][c], ofdm->Pend[c]));
        float phase_offset = rade_cangle(sum);

        /* Correct all symbols */
        for (int s = 0; s < Ns + 2; s++) {
            rx_sym[s][c] = rade_cmul(rx_sym[s][c], rade_cexp(-phase_offset));
        }
    }

    /* Extract data symbols (symbols 2 to Ns, i.e., Ns-1 symbols) */
    int out_idx = 0;
    for (int s = 2; s < Ns; s++) {
        for (int c = 0; c < Nc; c++) {
            z_hat[out_idx++] = rx_sym[s][c].real;
            z_hat[out_idx++] = rx_sym[s][c].imag;
        }
    }

    return out_idx;
}
