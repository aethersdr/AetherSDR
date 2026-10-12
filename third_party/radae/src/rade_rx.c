/*---------------------------------------------------------------------------*\

  rade_rx.c

  RADAE receiver - demodulates IQ samples to features.

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

#include "rade_rx.h"
#include "rade_dec_data.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

int rade_rx_init(rade_rx_state *rx, const RADEDec *dec_model, int bottleneck, int auxdata, int bpf_en) {
    memset(rx, 0, sizeof(rade_rx_state));

    rx->bottleneck = bottleneck;
    rx->auxdata = auxdata;
    rx->num_features = RADE_NUM_FEATURES + (auxdata ? 1 : 0);
    rx->bpf_en = bpf_en;
    rx->coarse_mag = 1;
    rx->time_offset = -16;  /* Default fine timing offset */
    rx->verbose = 2;

    /* Initialize OFDM demodulator */
    rade_ofdm_init(&rx->ofdm, bottleneck);

    /* Initialize acquisition */
    rade_acq_init(&rx->acq, &rx->ofdm, RADE_ACQ_FRANGE, RADE_ACQ_FSTEP);

    /* Initialize decoder if model provided */
    if (dec_model != NULL) {
        memcpy(&rx->dec_model, dec_model, sizeof(RADEDec));
    } else {
        /* Use built-in weights */
        int output_dim = rx->num_features * RADE_FRAMES_PER_STEP;
        if (init_radedec(&rx->dec_model, radedec_arrays, output_dim) != 0) {
            return -1;
        }
    }
    rade_init_decoder(&rx->dec_state);

    /* Initialize Rx BPF if enabled */
    if (bpf_en) {
        float w_min = rx->ofdm.w[0];
        float w_max = rx->ofdm.w[RADE_NC - 1];
        float bandwidth = 1.2f * (w_max - w_min) * RADE_FS / (2.0f * M_PI);
        float centre = (w_max + w_min) * RADE_FS / (2.0f * M_PI) / 2.0f;
        rade_bpf_init(&rx->bpf, RADE_BPF_NTAP, RADE_FS, bandwidth, centre, RADE_FS);
    }

    /* Initialize state machine */
    rx->state = RADE_STATE_SEARCH;
    rx->nin = RADE_NMF;
    rx->mf = 1;
    rx->rx_phase = rade_cone();

    /* Calculate unsync timeout (modem frames) */
    rx->Nmf_unsync = (int)(RADE_TUNSYNC * RADE_FS / RADE_NMF);
    rx->synced_count_one_sec = RADE_FS / RADE_NMF;

    /* Clear receive buffer */
    memset(rx->rx_buf, 0, sizeof(rx->rx_buf));

    return 0;
}

void rade_rx_reset(rade_rx_state *rx) {
    rx->state = RADE_STATE_SEARCH;
    rx->nin = RADE_NMF;
    rx->valid_count = 0;
    rx->synced_count = 0;
    rx->uw_errors = 0;
    rx->rx_phase = rade_cone();
    rx->snrdB_3k_est = 0.0f;
    rade_init_decoder(&rx->dec_state);
    if (rx->bpf_en) {
        rade_bpf_reset(&rx->bpf);
    }
    memset(rx->rx_buf, 0, sizeof(rx->rx_buf));
}

/*---------------------------------------------------------------------------*\
                           RECEPTION
\*---------------------------------------------------------------------------*/

int rade_rx_nin(const rade_rx_state *rx) {
    return rx->nin;
}

int rade_rx_nin_max(const rade_rx_state *rx) {
    return RADE_NMF + RADE_M;
}

int rade_rx_n_features_out(const rade_rx_state *rx) {
    return RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES;
}

int rade_rx_n_eoo_bits(const rade_rx_state *rx) {
    return (RADE_NS - 1) * RADE_NC * 2;
}

int rade_rx_sync(const rade_rx_state *rx) {
    return (rx->state == RADE_STATE_SYNC) ? 1 : 0;
}

float rade_rx_snrdB_3k_est(const rade_rx_state *rx) {
    return rx->snrdB_3k_est;
}

float rade_rx_freq_offset(const rade_rx_state *rx) {
    return rx->fmax;
}

void rade_rx_sum_uw_errors(rade_rx_state *rx, int new_uw_errors) {
    rx->uw_errors += new_uw_errors;
}

int rade_rx_process(rade_rx_state *rx, float *features_out, float *eoo_out, const RADE_COMP *rx_in) {
    int M = RADE_M;
    int Ncp = RADE_NCP;
    int Nmf = RADE_NMF;
    float Fs = (float)RADE_FS;

    int prev_state = rx->state;
    int valid_output = 0;
    int endofover = 0;
    int uw_fail = 0;

    /* Apply BPF if enabled */
    RADE_COMP rx_filtered[RADE_NMF + RADE_M];
    const RADE_COMP *rx_samples = rx_in;

    if (rx->bpf_en) {
        rade_bpf_process(&rx->bpf, rx_filtered, rx_in, rx->nin);
        rx_samples = rx_filtered;
    }

    /* Update receive buffer: shift out old samples, add new */
    int buf_size = RADE_RX_BUF_SIZE;
    if (rx->nin > 0)
    {
        memmove(rx->rx_buf, &rx->rx_buf[rx->nin], sizeof(RADE_COMP) * (buf_size - rx->nin));
        memcpy(&rx->rx_buf[buf_size - rx->nin], rx_samples, sizeof(RADE_COMP) * rx->nin);
    }

    /* State machine processing */
    int candidate = 0;
    int valid = 0;

    if (rx->state == RADE_STATE_SEARCH || rx->state == RADE_STATE_CANDIDATE) {
        /* Acquisition mode: detect pilots */
        candidate = rade_acq_detect_pilots(&rx->acq, rx->rx_buf, &rx->tmax, &rx->fmax);
    } else {
        /* Sync mode: refine timing/freq and check pilots */
        float ffine_start = rx->fmax - 1.0f;
        float ffine_end = rx->fmax + 1.0f;
        int tfine_start = (rx->tmax > 8) ? (rx->tmax - 8) : 0;
        int tfine_end = rx->tmax + 8;

        float fmax_hat = rx->fmax;
        rade_acq_refine(&rx->acq, rx->rx_buf, &rx->tmax, &fmax_hat,
                       tfine_start, tfine_end, ffine_start, ffine_end, 0.1f);

        /* Low-pass filter frequency estimate */
        rx->fmax = 0.9f * rx->fmax + 0.1f * fmax_hat;

        /* Check pilots */
        rade_acq_check_pilots(&rx->acq, rx->rx_buf, rx->tmax, rx->fmax, &candidate, &endofover);

        /* Handle timing slips */
        rx->nin = Nmf;
        if (rx->tmax >= Nmf - M) {
            rx->nin = Nmf + M;
            rx->tmax -= M;
        }
        if (rx->tmax < M) {
            rx->nin = Nmf - M;
            rx->tmax += M;
        }

        rx->synced_count++;

        /* Check UW errors once per second */
        if (rx->synced_count % rx->synced_count_one_sec == 0) {
            if (rx->uw_errors > RADE_UW_ERROR_THRESH) {
                uw_fail = 1;
            }
            rx->uw_errors = 0;
        }

        /* Frequency offset correction */
        float w = 2.0f * M_PI * rx->fmax / Fs;
        RADE_COMP rx_corrected[RADE_NMF + RADE_M + RADE_NCP];

        RADE_COMP rx_phase;
        for (int n = 0; n < Nmf + M + Ncp; n++) {
            rx_phase = rade_cmul(rx->rx_phase, rade_cexp(-w*(n+1)));
            rx_corrected[n] = rade_cmul(rx->rx_buf[rx->tmax - Ncp + n], rx_phase);
        }
        rx->rx_phase = rx_phase;

        /* Normalize phase to prevent drift */
        float phase_mag = rade_cabs(rx->rx_phase);
        rx->rx_phase = rade_cscale(rx->rx_phase, 1.0f / phase_mag);

        /* Demodulate OFDM frame */
        float z_hat[RADE_NZMF * RADE_LATENT_DIM];
        float snr_est = 0.0f;

        rade_ofdm_demod_frame(&rx->ofdm, z_hat, rx_corrected,
                              rx->time_offset, endofover, rx->coarse_mag, &snr_est);

        /* Update SNR estimate with moving average */
        rx->snrdB_3k_est = 0.9f * rx->snrdB_3k_est + 0.1f * snr_est;

        valid_output = !endofover;

        if (valid_output) {
            /* Decode latents to features */
            int Nzmf = RADE_NZMF;
            int latent_dim = RADE_LATENT_DIM;
            int dec_stride = RADE_FRAMES_PER_STEP;
            int num_used_features = RADE_NUM_FEATURES;
            int nb_total_features = RADE_NB_TOTAL_FEATURES;
            int num_features = rx->num_features;
            int arch = 0;

            /* Zero output buffer */
            int n_features_out = rade_rx_n_features_out(rx);
            memset(features_out, 0, sizeof(float) * n_features_out);

            int uw_errors_total = 0;

            for (int c = 0; c < Nzmf; c++) {
                float dec_features[RADE_FRAMES_PER_STEP * RADE_NUM_FEATURES_AUX];

                rade_core_decoder(&rx->dec_state, &rx->dec_model,
                                 dec_features, &z_hat[c * latent_dim], arch);

                /* Copy decoded features to output (with padding) */
                for (int i = 0; i < dec_stride; i++) {
                    int out_idx = (c * dec_stride + i) * nb_total_features;
                    for (int j = 0; j < num_used_features; j++) {
                        features_out[out_idx + j] = dec_features[i * num_features + j];
                    }
                }

                /* Check auxiliary data for unique word errors */
                if (rx->auxdata) {
                    /* Use first aux symbol of each group (they repeat) */
                    if (dec_features[num_used_features] > 0) {
                        uw_errors_total++;
                    }
                }
            }

            if (rx->auxdata) {
                rx->uw_errors += uw_errors_total;
            }
        }

        if (endofover) {
            /* Copy EOO symbols to output */
            float z_hat_eoo[(RADE_NS - 1) * RADE_NC * 2];
            rade_ofdm_demod_eoo(&rx->ofdm, z_hat_eoo, rx_corrected, rx->time_offset);

            int n_eoo_bits = rade_rx_n_eoo_bits(rx);
            memcpy(eoo_out, z_hat_eoo, sizeof(float) * n_eoo_bits);
        }
    }

    /* Verbose output */
    if (rx->verbose == 2 ||
        (rx->verbose == 1 && (rx->state == RADE_STATE_SEARCH ||
                              rx->state == RADE_STATE_CANDIDATE ||
                              prev_state == RADE_STATE_CANDIDATE))) {
        const char *state_str = (rx->state == RADE_STATE_SEARCH) ? "search" :
                                (rx->state == RADE_STATE_CANDIDATE) ? "candidate" : "sync";
        fprintf(stderr, "%3d state: %10s valid: %d %d %2d Dthresh: %8.2f ",
                rx->mf, state_str, candidate, endofover, rx->valid_count, rx->acq.Dthresh);
        fprintf(stderr, "Dtmax12: %8.2f %8.2f tmax: %4d fmax: %6.2f",
                rx->acq.Dtmax12, rx->acq.Dtmax12_eoo, rx->tmax, rx->fmax);
        fprintf(stderr, " SNRdB: %5.2f", rx->snrdB_3k_est);
        if (rx->auxdata && rx->state == RADE_STATE_SYNC) {
            fprintf(stderr, " uw_err: %d", rx->uw_errors);
        }
        fprintf(stderr, "\n");
    }

    /* State machine transitions */
    int next_state = rx->state;

    if (rx->state == RADE_STATE_SEARCH) {
        if (candidate) {
            next_state = RADE_STATE_CANDIDATE;
            rx->tmax_candidate = rx->tmax;
            rx->valid_count = 1;
        }
    } else if (rx->state == RADE_STATE_CANDIDATE) {
        /* Look for 3 consecutive matches with similar timing */
        if (candidate && abs(rx->tmax - rx->tmax_candidate) < Ncp) {
            rx->valid_count++;
            if (rx->valid_count > 3) {
                next_state = RADE_STATE_SYNC;
                rade_init_decoder(&rx->dec_state);  /* Reset decoder state */
                rx->synced_count = 0;
                rx->uw_errors = 0;
                rx->valid_count = rx->Nmf_unsync;

                /* Fine refinement of timing/frequency */
                float ffine_start = rx->fmax - 10.0f;
                float ffine_end = rx->fmax + 10.0f;
                int tfine_start = (rx->tmax > 1) ? (rx->tmax - 1) : 0;
                int tfine_end = rx->tmax + 2;

                rade_acq_refine(&rx->acq, rx->rx_buf, &rx->tmax, &rx->fmax,
                               tfine_start, tfine_end, ffine_start, ffine_end, 0.25f);
            }
        } else {
            next_state = RADE_STATE_SEARCH;
        }
    } else if (rx->state == RADE_STATE_SYNC) {
        /* During some tests it's useful to disable unsync features */
        int unsync_enable = 1;
        if (rx->disable_unsync > 0.0f) {
            int disable_after_frames = (int)(rx->disable_unsync * Fs / Nmf);
            if (rx->synced_count > disable_after_frames) {
                unsync_enable = 0;
            }
        }

        if (candidate) {
            rx->valid_count = rx->Nmf_unsync;
        } else {
            rx->valid_count--;
            if (unsync_enable && rx->valid_count == 0) {
                next_state = RADE_STATE_SEARCH;
            }
        }

        if (unsync_enable && (endofover || uw_fail)) {
            next_state = RADE_STATE_SEARCH;
        }
    }

    rx->state = next_state;
    if (rx->state == RADE_STATE_SEARCH) {
        rx->nin = Nmf;  /* Reset nin when not synced */
    }
    rx->mf++;

    /* Return flags */
    return (valid_output ? 0x1 : 0) | (endofover ? 0x2 : 0);
}
