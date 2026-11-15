/*---------------------------------------------------------------------------*\

  rade_tx.h

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

#ifndef __RADE_TX__
#define __RADE_TX__

#include "rade_dsp.h"
#include "rade_ofdm.h"
#include "rade_bpf.h"
#include "rade_enc.h"
#include "rade_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                           TRANSMITTER STATE
\*---------------------------------------------------------------------------*/

typedef struct {
    /* OFDM modulator */
    rade_ofdm ofdm;

    /* Optional Tx BPF */
    rade_bpf bpf;
    int bpf_en;

    /* Core encoder */
    RADEEnc enc_model;
    RADEEncState enc_state;

    /* Configuration */
    int bottleneck;
    int auxdata;
    int num_features;       /* 20 or 21 (with auxdata) */

    /* EOO bits (for supplementary data channel) */
    float eoo_bits[RADE_NC * (RADE_NS - 1) * 2];  /* Nseoo * 2 */
    int n_eoo_bits;

} rade_tx_state;

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Initialize transmitter
   enc_model: pointer to encoder model weights (can be NULL to skip encoder init)
   bottleneck: 1, 2, or 3 (PA saturation model)
   auxdata: 1 to enable auxiliary data symbols
   bpf_en: 1 to enable Tx bandpass filter
   Returns 0 on success */
int rade_tx_init(rade_tx_state *tx, const RADEEnc *enc_model, int bottleneck, int auxdata, int bpf_en);

/* Reset transmitter state (clear encoder state) */
void rade_tx_reset(rade_tx_state *tx);

/*---------------------------------------------------------------------------*\
                           TRANSMISSION
\*---------------------------------------------------------------------------*/

/* Get number of input features per modem frame */
int rade_tx_n_features_in(const rade_tx_state *tx);

/* Get number of output samples per modem frame */
int rade_tx_n_samples_out(const rade_tx_state *tx);

/* Get number of output samples for EOO frame */
int rade_tx_n_eoo_out(const rade_tx_state *tx);

/* Get number of EOO bits */
int rade_tx_n_eoo_bits(const rade_tx_state *tx);

/* Set EOO bits (in +/-1 float form)
   bits: array of n_eoo_bits floats (+1 or -1) */
void rade_tx_state_set_eoo_bits(rade_tx_state *tx, const float *bits);

/* Transmit one modem frame
   features_in: input features [n_features_in] in padded format (36 floats per frame)
   tx_out: output IQ samples [n_samples_out]
   Returns number of samples written to tx_out */
int rade_tx_process(rade_tx_state *tx, RADE_COMP *tx_out, const float *features_in);

/* Transmit end-of-over frame
   tx_out: output IQ samples [n_eoo_out]
   Returns number of samples written */
int rade_tx_state_eoo(rade_tx_state *tx, RADE_COMP *tx_out);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_TX__ */
