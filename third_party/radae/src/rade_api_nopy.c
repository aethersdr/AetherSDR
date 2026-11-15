/*---------------------------------------------------------------------------*\

  rade_api_nopy.c

  Library of API functions that implement the Radio Autoencoder API.
  Pure C implementation - no Python dependency.

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

#define VERSION 2  /* Bump when API changes; version 2 = Python-free */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rade_api.h"

/*---------------------------------------------------------------------------*\
                        INITIALIZATION
\*---------------------------------------------------------------------------*/

void rade_initialize(void) {
    /* No initialization needed without Python */
}

void rade_finalize(void) {
    /* No finalization needed without Python */
}

struct rade *rade_open(char model_file[], int flags) {
    struct rade *r = (struct rade *)malloc(sizeof(struct rade));
    if (r == NULL) {
        fprintf(stderr, "rade_open: failed to allocate memory\n");
        return NULL;
    }
    memset(r, 0, sizeof(struct rade));

    r->flags = flags;
    r->auxdata = 1;
    r->bottleneck = 3;

    /* Note: model_file is ignored in this implementation
       Weights are compiled in via rade_enc_data.c and rade_dec_data.c */
    fprintf(stderr, "rade_open: model_file=%s (ignored, using built-in weights)\n", model_file);

    /* Initialize transmitter
       RADE_USE_C_ENCODER flag is now always implicitly set */
    int bpf_en = 0;  /* BPF disabled by default */
    if (rade_tx_init(&r->tx, NULL, r->bottleneck, r->auxdata, bpf_en) != 0) {
        fprintf(stderr, "rade_open: failed to initialize transmitter\n");
        free(r);
        return NULL;
    }

    /* Initialize receiver
       RADE_USE_C_DECODER flag is now always implicitly set */
    if (rade_rx_init(&r->rx, NULL, r->bottleneck, r->auxdata, 1) != 0) {
        fprintf(stderr, "rade_open: failed to initialize receiver\n");
        free(r);
        return NULL;
    }

    /* Set verbosity based on flags */
    if (flags & RADE_VERBOSE_0) {
        r->rx.verbose = 0;
    }

    fprintf(stderr, "rade_open: n_features_in=%d Nmf=%d Neoo=%d n_eoo_bits=%d\n",
            rade_tx_n_features_in(&r->tx),
            rade_tx_n_samples_out(&r->tx),
            rade_tx_n_eoo_out(&r->tx),
            rade_tx_n_eoo_bits(&r->tx));

    return r;
}

void rade_close(struct rade *r) {
    if (r != NULL) {
        free(r);
    }
}

/*---------------------------------------------------------------------------*\
                         GETTERS
\*---------------------------------------------------------------------------*/

int rade_version(void) {
    return VERSION;
}

int rade_n_tx_out(struct rade *r) {
    assert(r != NULL);
    return rade_tx_n_samples_out(&r->tx);
}

int rade_n_tx_eoo_out(struct rade *r) {
    assert(r != NULL);
    return rade_tx_n_eoo_out(&r->tx);
}

int rade_nin_max(struct rade *r) {
    assert(r != NULL);
    return rade_rx_nin_max(&r->rx);
}

int rade_nin(struct rade *r) {
    assert(r != NULL);
    return rade_rx_nin(&r->rx);
}

int rade_n_features_in_out(struct rade *r) {
    assert(r != NULL);
    return rade_tx_n_features_in(&r->tx);
}

int rade_n_eoo_bits(struct rade *r) {
    assert(r != NULL);
    return rade_tx_n_eoo_bits(&r->tx);
}

/*---------------------------------------------------------------------------*\
                         TRANSMISSION
\*---------------------------------------------------------------------------*/

RADE_EXPORT void rade_tx_set_eoo_bits(struct rade *r, float eoo_bits[]) {
    assert(r != NULL);
    assert(eoo_bits != NULL);
    rade_tx_state_set_eoo_bits(&r->tx, eoo_bits);
}

RADE_EXPORT void rade_tx_set_eoo_callsign(struct rade *r, const char *callsign) {
    assert(r != NULL);
    assert(callsign != NULL);
    assert(rade_tx_n_eoo_bits(&r->tx) >= RADE_EOO_CALLSIGN_MAX * 7);

    int src_len = (int)strlen(callsign);
    for (int i = 0; i < RADE_EOO_CALLSIGN_MAX; i++) {
        unsigned char c = (i < src_len) ? (unsigned char)callsign[i] : ' ';
        /* Store 7 bits MSB-first (+1.0 = 1, -1.0 = 0) */
        for (int b = 0; b < 7; b++) {
            int bit = (c >> (6 - b)) & 1;
            r->tx.eoo_bits[i * 7 + b] = bit ? 1.0f : -1.0f;
        }
    }
}

RADE_EXPORT int rade_rx_get_eoo_callsign(const float *eoo_bits, int n_eoo_bits,
                                          char *callsign_out) {
    assert(eoo_bits != NULL);
    assert(callsign_out != NULL);

    if (n_eoo_bits < RADE_EOO_CALLSIGN_MAX * 7) {
        callsign_out[0] = '\0';
        return 0;
    }

    for (int i = 0; i < RADE_EOO_CALLSIGN_MAX; i++) {
        unsigned char c = 0;
        for (int b = 0; b < 7; b++) {
            int bit = (eoo_bits[i * 7 + b] > 0.0f) ? 1 : 0;
            c |= (unsigned char)(bit << (6 - b));
        }
        callsign_out[i] = (char)c;
    }
    callsign_out[RADE_EOO_CALLSIGN_MAX] = '\0';

    /* Trim trailing spaces */
    int len = RADE_EOO_CALLSIGN_MAX;
    while (len > 0 && callsign_out[len - 1] == ' ')
        callsign_out[--len] = '\0';

    return len;
}

int rade_tx(struct rade *r, RADE_COMP tx_out[], float features_in[]) {
    assert(r != NULL);
    assert(features_in != NULL);
    assert(tx_out != NULL);

    return rade_tx_process(&r->tx, tx_out, features_in);
}

int rade_tx_eoo(struct rade *r, RADE_COMP tx_eoo_out[]) {
    assert(r != NULL);
    assert(tx_eoo_out != NULL);

    return rade_tx_state_eoo(&r->tx, tx_eoo_out);
}

/*---------------------------------------------------------------------------*\
                         RECEPTION
\*---------------------------------------------------------------------------*/

int rade_rx(struct rade *r, float features_out[], int *has_eoo_out, float eoo_out[], RADE_COMP rx_in[]) {
    assert(r != NULL);
    assert(features_out != NULL);
    assert(rx_in != NULL);

    int ret = rade_rx_process(&r->rx, features_out, eoo_out, rx_in);

    int valid_out = ret & 0x1;
    int endofover = ret & 0x2;

    *has_eoo_out = endofover ? 1 : 0;

    if (valid_out) {
        return rade_rx_n_features_out(&r->rx);
    } else {
        return 0;
    }
}

int rade_sync(struct rade *r) {
    assert(r != NULL);
    return rade_rx_sync(&r->rx);
}

float rade_freq_offset(struct rade *r) {
    assert(r != NULL);
    return rade_rx_freq_offset(&r->rx);
}

int rade_snrdB_3k_est(struct rade *r) {
    assert(r != NULL);
    return (int)rade_rx_snrdB_3k_est(&r->rx);
}

void rade_set_disable_unsync(struct rade *r, float seconds) {
    assert(r != NULL);
    r->rx.disable_unsync = seconds;
}
