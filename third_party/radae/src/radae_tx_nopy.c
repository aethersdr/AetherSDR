/*---------------------------------------------------------------------------*\

  radae_tx_nopy.c

  RADAE streaming transmitter - Python-free version.
  Reads features from stdin, writes IQ samples to stdout.

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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "rade_api.h"
#include "rade_dsp.h"

void usage(void) {
    fprintf(stderr, "usage: radae_tx_nopy [options]\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
    fprintf(stderr, "  --model_name FILE    Path to model (ignored, uses built-in weights)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Reads vocoder features from stdin, writes IQ samples to stdout.\n");
    fprintf(stderr, "Features format: float32, %d values per modem frame\n",
            RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES);
    fprintf(stderr, "Output format: complex float32 (interleaved I,Q), %d samples per modem frame\n",
            RADE_NMF);
}

int main(int argc, char *argv[]) {
    int opt;
    char *model_name = "model19_check3/checkpoints/checkpoint_epoch_100.pth";

    static struct option long_options[] = {
        {"help",       no_argument,       NULL, 'h'},
        {"model_name", required_argument, NULL, 'm'},
        {NULL,         0,                 NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "hm:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case 'm':
            model_name = optarg;
            break;
        default:
            usage();
            return 1;
        }
    }

    /* Initialize RADE */
    rade_initialize();

    struct rade *r = rade_open(model_name, 0);
    if (r == NULL) {
        fprintf(stderr, "Failed to open RADE\n");
        return 1;
    }

    int n_features_in = rade_n_features_in_out(r);
    int n_tx_out = rade_n_tx_out(r);
    int n_eoo_out = rade_n_tx_eoo_out(r);

    FILE *feoo_bits = fopen("eoo_tx.f32","rb");
    if (feoo_bits) {
        fprintf(stderr, "Setting EOO\n");
        int n_eoo_bits = rade_n_eoo_bits(r);
        float eoo_bits[n_eoo_bits];
        int ret = fread(eoo_bits, sizeof(float), n_eoo_bits, feoo_bits);
        assert(ret == n_eoo_bits);
        rade_tx_set_eoo_bits(r, eoo_bits);
        fclose(feoo_bits);
    }
    
    fprintf(stderr, "n_features_in: %d n_tx_out: %d n_eoo_out: %d\n",
            n_features_in, n_tx_out, n_eoo_out);

    /* Allocate buffers */
    float *features_in = (float *)malloc(sizeof(float) * n_features_in);
    RADE_COMP *tx_out = (RADE_COMP *)malloc(sizeof(RADE_COMP) * n_tx_out);
    RADE_COMP *eoo_out = (RADE_COMP *)malloc(sizeof(RADE_COMP) * n_eoo_out);

    if (features_in == NULL || tx_out == NULL || eoo_out == NULL) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return 1;
    }

    /* Main processing loop */
    int frame_count = 0;
    while (1) {
        size_t n_read = fread(features_in, sizeof(float), n_features_in, stdin);
        if (n_read != (size_t)n_features_in) {
            break;
        }

        /* Transmit features */
        int n_out = rade_tx(r, tx_out, features_in);
        fwrite(tx_out, sizeof(RADE_COMP), n_out, stdout);
        frame_count++;
    }

    /* Send end-of-over frame */
    int n_out = rade_tx_eoo(r, eoo_out);
    fwrite(eoo_out, sizeof(RADE_COMP), n_out, stdout);

    // extra silence buf to let Rx finish processing EOO
    memset(eoo_out,0,sizeof(eoo_out));
    fwrite(eoo_out, sizeof(RADE_COMP), n_out, stdout);

    fprintf(stderr, "Transmitted %d modem frames + EOO\n", frame_count);

    /* Cleanup */
    free(features_in);
    free(tx_out);
    free(eoo_out);
    rade_close(r);
    rade_finalize();

    return 0;
}
