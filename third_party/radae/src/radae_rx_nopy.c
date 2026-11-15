/*---------------------------------------------------------------------------*\

  radae_rx_nopy.c

  RADAE streaming receiver - Python-free version.
  Reads IQ samples from stdin, writes features to stdout.

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "rade_api.h"
#include "rade_dsp.h"

void usage(void) {
    fprintf(stderr, "usage: radae_rx_nopy [options]\n");
    fprintf(stderr, "  -h, --help              Show this help\n");
    fprintf(stderr, "  --model_name FILE       Path to model (ignored, uses built-in weights)\n");
    fprintf(stderr, "  -v LEVEL                Verbosity level (0, 1, or 2)\n");
    fprintf(stderr, "  --disable_unsync SECS   Test mode: disable unsync after SECS seconds (default 0 = disabled)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Reads IQ samples from stdin, writes vocoder features to stdout.\n");
    fprintf(stderr, "Input format: complex float32 (interleaved I,Q)\n");
    fprintf(stderr, "Output format: float32 features, %d values per modem frame\n",
            RADE_NZMF * RADE_FRAMES_PER_STEP * RADE_NB_TOTAL_FEATURES);
}

int main(int argc, char *argv[]) {
    int opt;
    char *model_name = "model19_check3/checkpoints/checkpoint_epoch_100.pth";
    int flags = 0;
    float disable_unsync = 0.0f;

    static struct option long_options[] = {
        {"help",           no_argument,       NULL, 'h'},
        {"model_name",     required_argument, NULL, 'm'},
        {"disable_unsync", required_argument, NULL, 'd'},
        {NULL,             0,                 NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "hm:v:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            return 0;
        case 'm':
            model_name = optarg;
            break;
        case 'v':
            if (atoi(optarg) == 0) {
                flags |= RADE_VERBOSE_0;
            }
            break;
        case 'd':
            disable_unsync = atof(optarg);
            break;
        default:
            usage();
            return 1;
        }
    }

    /* Initialize RADE */
    rade_initialize();

    struct rade *r = rade_open(model_name, flags);
    if (r == NULL) {
        fprintf(stderr, "Failed to open RADE\n");
        return 1;
    }

    /* Set test mode options */
    if (disable_unsync > 0.0f) {
        rade_set_disable_unsync(r, disable_unsync);
        fprintf(stderr, "disable_unsync: %.1f seconds\n", disable_unsync);
    }

    int nin_max = rade_nin_max(r);
    int n_features_out = rade_n_features_in_out(r);
    int n_eoo_bits = rade_n_eoo_bits(r);

    fprintf(stderr, "nin_max: %d n_features_out: %d n_eoo_bits: %d\n",
            nin_max, n_features_out, n_eoo_bits);

    /* Allocate buffers */
    RADE_COMP *rx_in = (RADE_COMP *)malloc(sizeof(RADE_COMP) * nin_max);
    float *features_out = (float *)malloc(sizeof(float) * n_features_out);
    float *eoo_out = (float *)malloc(sizeof(float) * n_eoo_bits);

    if (rx_in == NULL || features_out == NULL || eoo_out == NULL) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return 1;
    }

    FILE *feoo_bits = fopen("eoo_rx.f32","wb");

    /* Main processing loop */
    int frame_count = 0;
    int valid_count = 0;
    while (1) {
        int nin = rade_nin(r);
        size_t n_read = fread(rx_in, sizeof(RADE_COMP), nin, stdin);
        if (n_read != (size_t)nin) {
            break;
        }

        /* Receive samples */
        int has_eoo = 0;
        int n_out = rade_rx(r, features_out, &has_eoo, eoo_out, rx_in);

        if (n_out > 0) {
            fwrite(features_out, sizeof(float), n_out, stdout);
            valid_count++;
        }

        if (has_eoo) {
            fprintf(stderr, "End-of-over detected\n");
            if (feoo_bits) {
                fwrite(eoo_out, sizeof(float), n_eoo_bits, feoo_bits);
            }
        }

        frame_count++;
    }

    fprintf(stderr, "Processed %d modem frames, %d valid outputs\n", frame_count, valid_count);

    /* Cleanup */
    if (feoo_bits) {
        fclose(feoo_bits);
    }
    free(rx_in);
    free(features_out);
    free(eoo_out);
    rade_close(r);
    rade_finalize();

    return 0;
}
