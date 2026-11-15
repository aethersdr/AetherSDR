/*---------------------------------------------------------------------------*\

  rade_tx.c

  RADAE transmitter - encodes features to IQ samples.

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

#include "rade_tx.h"
#include "rade_enc_data.h"
#include <string.h>
#include <assert.h>

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

int rade_tx_init(rade_tx_state *tx, const RADEEnc *enc_model, int bottleneck, int auxdata, int bpf_en) {
    memset(tx, 0, sizeof(rade_tx_state));

    tx->bottleneck = bottleneck;
    tx->auxdata = auxdata;
    tx->num_features = RADE_NUM_FEATURES + (auxdata ? 1 : 0);
    tx->bpf_en = bpf_en;

    /* Initialize OFDM modulator */
    rade_ofdm_init(&tx->ofdm, bottleneck);

    /* Initialize encoder if model provided */
    if (enc_model != NULL) {
        memcpy(&tx->enc_model, enc_model, sizeof(RADEEnc));
    } else {
        /* Use built-in weights */
        int input_dim = tx->num_features * RADE_FRAMES_PER_STEP;
        if (init_radeenc(&tx->enc_model, radeenc_arrays, input_dim) != 0) {
            return -1;
        }
    }
    rade_init_encoder(&tx->enc_state);

    /* Initialize Tx BPF if enabled */
    if (bpf_en) {
        /* Calculate bandwidth from carrier frequencies */
        float w_min = tx->ofdm.w[0];
        float w_max = tx->ofdm.w[RADE_NC - 1];
        float bandwidth = 1.2f * (w_max - w_min) * RADE_FS / (2.0f * M_PI);
        float centre = (w_max + w_min) * RADE_FS / (2.0f * M_PI) / 2.0f;
        rade_bpf_init(&tx->bpf, RADE_BPF_NTAP, RADE_FS, bandwidth, centre, RADE_FS);
    }

    /* EOO bits count: (Ns-1) * Nc * 2 (QPSK symbols) */
    tx->n_eoo_bits = (RADE_NS - 1) * RADE_NC * 2;

    /* Initialize EOO bits to zero */
    memset(tx->eoo_bits, 0, sizeof(tx->eoo_bits));

    return 0;
}

void rade_tx_reset(rade_tx_state *tx) {
    rade_init_encoder(&tx->enc_state);
    if (tx->bpf_en) {
        rade_bpf_reset(&tx->bpf);
    }
}

/*---------------------------------------------------------------------------*\
                           TRANSMISSION
\*---------------------------------------------------------------------------*/

int rade_tx_n_features_in(const rade_tx_state *tx) {
    /* Features per modem frame: Nzmf * enc_stride * nb_total_features */
    return RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES;
}

int rade_tx_n_samples_out(const rade_tx_state *tx) {
    return RADE_NMF;
}

int rade_tx_n_eoo_out(const rade_tx_state *tx) {
    return RADE_NEOO;
}

int rade_tx_n_eoo_bits(const rade_tx_state *tx) {
    return tx->n_eoo_bits;
}

void rade_tx_state_set_eoo_bits(rade_tx_state *tx, const float *bits) {
    memcpy(tx->eoo_bits, bits, sizeof(float) * tx->n_eoo_bits);
}

int rade_tx_process(rade_tx_state *tx, RADE_COMP *tx_out, const float *features_in) {
    int Nzmf = RADE_NZMF;
    int enc_stride = RADE_FRAMES_PER_STEP;
    int latent_dim = RADE_LATENT_DIM;
    int num_features = tx->num_features;
    int num_used_features = RADE_NUM_FEATURES;
    int nb_total_features = RADE_NB_TOTAL_FEATURES;
    int arch = 0;  /* CPU architecture for optimized routines */

    /* Number of encoder calls per modem frame */
    int n_feature_vecs = Nzmf * enc_stride;
    int n_core_encoder = n_feature_vecs / enc_stride;  /* = Nzmf */

    /* Latent vectors output buffer */
    float z[RADE_NZMF * RADE_LATENT_DIM];

    /* Process each group of FRAMES_PER_STEP features through encoder */
    for (int c = 0; c < n_core_encoder; c++) {
        /* Extract and reformat features for encoder
           Input format: [frame][feature] where feature is padded to 36
           Encoder format: [frame][num_features] where num_features is 20 or 21 */
        float enc_features[RADE_FRAMES_PER_STEP * RADE_NUM_FEATURES_AUX];

        for (int i = 0; i < enc_stride; i++) {
            int in_idx = (c * enc_stride + i) * nb_total_features;

            /* Copy used features */
            for (int j = 0; j < num_used_features; j++) {
                enc_features[i * num_features + j] = features_in[in_idx + j];
            }

            /* Add auxiliary data symbol if enabled */
            if (tx->auxdata) {
                enc_features[i * num_features + num_used_features] = -1.0f;
            }
        }

        /* Run core encoder */
        rade_core_encoder(&tx->enc_state, &tx->enc_model,
                         &z[c * latent_dim], enc_features, arch, tx->bottleneck);
    }

    /* Modulate latent vectors to IQ samples */
    int n_out = rade_ofdm_mod_frame(&tx->ofdm, tx_out, z);

    /* Apply Tx BPF if enabled */
    if (tx->bpf_en) {
        RADE_COMP tx_filtered[RADE_NMF];
        rade_bpf_process(&tx->bpf, tx_filtered, tx_out, n_out);

        /* Clip magnitude to 1 after filtering */
        for (int i = 0; i < n_out; i++) {
            float mag = rade_cabs(tx_filtered[i]);
            if (mag > 1.0f) {
                tx_out[i] = rade_cscale(tx_filtered[i], 1.0f / mag);
            } else {
                tx_out[i] = tx_filtered[i];
            }
        }
    }

    return n_out;
}

int rade_tx_state_eoo(rade_tx_state *tx, RADE_COMP *tx_out) {
    int n_eoo;
    const RADE_COMP *eoo = rade_ofdm_get_eoo(&tx->ofdm, &n_eoo);

    /* Copy pre-computed EOO frame (pilots in place, data slots are zeros) */
    memcpy(tx_out, eoo, sizeof(RADE_COMP) * n_eoo);

    /* Modulate eoo_bits into the data symbol slots (positions 2..Ns).
       The pre-computed frame has zeros there; overwrite with the QPSK
       symbols encoded from the callsign (or all-zero if none was set).
       eoo_bits layout: [(Ns-1) data symbols][Nc carriers][I,Q] */
    {
        int Nc  = tx->ofdm.nc;
        int M   = tx->ofdm.m;
        int Ncp = tx->ofdm.ncp;
        int Ns  = tx->ofdm.ns;
        int n_data = Ns - 1;   /* data slots at frame positions 2..Ns */

        RADE_COMP freq_sym[RADE_NC];
        RADE_COMP time_sym[RADE_M];

        for (int d = 0; d < n_data; d++) {
            int frame_pos = d + 2;   /* symbol index in EOO frame */

            /* Build frequency-domain symbol from stored eoo_bits */
            for (int c = 0; c < Nc; c++) {
                int bit_idx = (d * Nc + c) * 2;
                freq_sym[c].real = tx->eoo_bits[bit_idx];
                freq_sym[c].imag = tx->eoo_bits[bit_idx + 1];
            }

            /* IDFT → time domain, insert CP, write to tx_out */
            rade_ofdm_idft(&tx->ofdm, time_sym, freq_sym);
            for (int c = 0; c < RADE_M; c++)
            {
                time_sym[c] = rade_cscale(time_sym[c], tx->ofdm.pilot_gain);
                if (tx->ofdm.bottleneck == 3) {
                    time_sym[c] = rade_tanh_limit(time_sym[c]);
                }
            }
            rade_ofdm_insert_cp(&tx->ofdm, &tx_out[frame_pos * (M + Ncp)], time_sym);
        }
    }

    /* Apply Tx BPF if enabled */
    if (tx->bpf_en) {
        RADE_COMP tx_filtered[RADE_NEOO];
        rade_bpf_process(&tx->bpf, tx_filtered, tx_out, n_eoo);

        /* Clip magnitude to 1 after filtering */
        for (int i = 0; i < n_eoo; i++) {
            float mag = rade_cabs(tx_filtered[i]);
            if (mag > 1.0f) {
                tx_out[i] = rade_cscale(tx_filtered[i], 1.0f / mag);
            } else {
                tx_out[i] = tx_filtered[i];
            }
        }
    }

    return n_eoo;
}
