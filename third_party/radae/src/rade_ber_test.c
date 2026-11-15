/*---------------------------------------------------------------------------*\

  rade_ber_test.c

  BER test for the RADE OFDM modem.  Substitutes random QPSK symbols (z = +/-1
  per component) for encoder output, passes through an AWGN channel with
  optional two-path Rayleigh fading (g_file), demodulates, and reports BER.

  The noise variance follows the same convention as inference.py rate_Fs mode
  (non-bottleneck-3):
      sigma = 1 / sqrt(EbNo * M)
  where EbNo = 10^(EbNodB/10) and M = RADE_M = 160.

  Usage:
      rade_ber_test [--EbNodB <dB>] [--g_file <path>] [--frames <N>]
                    [--seed <N>]

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe

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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_dsp.h"
#include "rade_ofdm.h"

#define N_FRAMES_DEFAULT 200
#define MULTIPATH_DELAY_S 0.002f  /* 2 ms, matches inference.py */

/*---------------------------------------------------------------------------*\
                          RANDOM NUMBER UTILITIES
\*---------------------------------------------------------------------------*/

/* xorshift32 PRBS — seeded via seed_prbs() */
static uint32_t prbs_state = 0x12345678u;

static void seed_prbs(uint32_t seed) { prbs_state = seed ? seed : 0x12345678u; }

static uint32_t xorshift32(void) {
    prbs_state ^= prbs_state << 13;
    prbs_state ^= prbs_state >> 17;
    prbs_state ^= prbs_state << 5;
    return prbs_state;
}

/* Return +1.0 or -1.0 pseudo-randomly */
static float prbs_pm1(void) { return (xorshift32() & 1u) ? 1.0f : -1.0f; }

/* Box-Muller: returns a standard normal sample N(0,1) */
static float randn(void) {
    static int have_spare = 0;
    static float spare = 0.0f;
    if (have_spare) { have_spare = 0; return spare; }
    have_spare = 1;
    float u, v, s;
    do {
        u = 2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f;
        v = 2.0f * ((float)rand() / (float)RAND_MAX) - 1.0f;
        s = u * u + v * v;
    } while (s >= 1.0f || s <= 0.0f);
    float mul = sqrtf(-2.0f * logf(s) / s);
    spare = v * mul;
    return u * mul;
}

/*---------------------------------------------------------------------------*\
                          G-FILE MULTIPATH HELPERS
\*---------------------------------------------------------------------------*/

typedef struct {
    RADE_COMP *G;   /* G[2*i] = G1[i], G[2*i+1] = G2[i] */
    int        len; /* number of time steps (pairs) */
} gfile_t;

/* Load g_file.  Returns 1 on success, 0 on failure.
   File format (complex float32):
     [0]          = (mp_gain, 0)   -- scalar normalisation factor
     [1,2]        = G1[0], G2[0]  -- first tap pair
     [3,4]        = G1[1], G2[1]
     ...
   The caller frees g->G when done. */
static int gfile_load(const char *path, gfile_t *g) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rade_ber_test: cannot open %s\n", path); return 0; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    int n_complex = (int)(fsize / (2 * sizeof(float)));
    RADE_COMP *raw = (RADE_COMP *)malloc(n_complex * sizeof(RADE_COMP));
    if (!raw) { fprintf(stderr, "rade_ber_test: out of memory\n"); fclose(f); return 0; }

    if ((int)fread(raw, 2 * sizeof(float), n_complex, f) != n_complex) {
        fprintf(stderr, "rade_ber_test: short read on %s\n", path);
        free(raw); fclose(f); return 0;
    }
    fclose(f);

    float mp_gain = raw[0].real;
    int n_pairs   = (n_complex - 1) / 2;

    g->G   = (RADE_COMP *)malloc(2 * n_pairs * sizeof(RADE_COMP));
    g->len = n_pairs;
    if (!g->G) { fprintf(stderr, "rade_ber_test: out of memory\n"); free(raw); return 0; }

    for (int i = 0; i < n_pairs; i++) {
        g->G[2*i+0] = rade_cscale(raw[1 + 2*i + 0], mp_gain);
        g->G[2*i+1] = rade_cscale(raw[1 + 2*i + 1], mp_gain);
    }
    free(raw);
    return 1;
}

/* Apply two-path multipath channel (in-place on tx, length n_tx).
   tx_mp[n] = G1[n]*tx[n] + G2[n-d]*tx[n-d]  (n >= d)
   Power is then normalised to match the pre-multipath power, matching
   the radae.py normalisation. */
static void apply_multipath(RADE_COMP *tx, int n_tx, const gfile_t *g) {
    int d = (int)(MULTIPATH_DELAY_S * (float)RADE_FS); /* 16 samples */

    RADE_COMP *tx_mp = (RADE_COMP *)calloc(n_tx, sizeof(RADE_COMP));
    assert(tx_mp);

    /* Compute pre-multipath signal power */
    float tx_power = 0.0f;
    for (int n = 0; n < n_tx; n++) tx_power += rade_cabs2(tx[n]);
    tx_power /= (float)n_tx;

    /* Apply two-path model */
    for (int n = 0; n < n_tx; n++) {
        int gi  = n % g->len;
        tx_mp[n] = rade_cmul(tx[n], g->G[2*gi + 0]);
        if (n >= d) {
            int gi2 = (n - d) % g->len;
            tx_mp[n] = rade_cadd(tx_mp[n],
                                 rade_cmul(tx[n - d], g->G[2*gi2 + 1]));
        }
    }

    /* Normalise power to match pre-channel level */
    float tx_mp_power = 0.0f;
    for (int n = 0; n < n_tx; n++) tx_mp_power += rade_cabs2(tx_mp[n]);
    tx_mp_power /= (float)n_tx;

    float scale = sqrtf(tx_power / (tx_mp_power + 1e-12f));
    for (int n = 0; n < n_tx; n++) tx[n] = rade_cscale(tx_mp[n], scale);

    free(tx_mp);
}

/*---------------------------------------------------------------------------*\
                                  MAIN
\*---------------------------------------------------------------------------*/

int main(int argc, char *argv[]) {
    /* ---- defaults ---- */
    float  EbNodB      = 0.0f;
    char  *g_file_path = NULL;
    int    n_frames    = N_FRAMES_DEFAULT;
    uint32_t seed      = 0;

    /* ---- argument parsing ---- */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--EbNodB") && i+1 < argc)
            EbNodB = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--g_file") && i+1 < argc)
            g_file_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i+1 < argc)
            n_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i+1 < argc)
            seed = (uint32_t)atoi(argv[++i]);
    }

    /* ---- random seeds ---- */
    seed_prbs(seed);
    srand(seed ? seed : 42);

    /* ---- OFDM init (bottleneck=1: no PA saturation) ---- */
    rade_ofdm ofdm;
    rade_ofdm_init(&ofdm, 1);

    int Nmf        = RADE_NMF;               /* 960 samples/modem frame  */
    int M          = RADE_M;                 /* 160                       */
    int Ncp        = RADE_NCP;               /* 32                        */
    int z_per_frame = RADE_NZMF * RADE_LATENT_DIM; /* 3*80 = 240           */

    /* ---- AWGN noise parameters ----
       Matches inference.py rate_Fs (non-bottleneck-3):
           sigma = 1 / sqrt(EbNo * M)
       Python uses complex randn_like where each of real/imag has variance 0.5,
       so total noise power = sigma^2.  In C we draw real/imag each from N(0,1)
       and scale by sigma/sqrt(2) to obtain the same noise power. */
    float EbNo      = powf(10.0f, EbNodB / 10.0f);
    float sigma     = 1.0f / sqrtf(EbNo * (float)M);
    float noise_std = sigma / sqrtf(2.0f); /* per real / imag component */

    /* ---- load g_file (optional) ---- */
    gfile_t gfile = {NULL, 0};
    if (g_file_path && !gfile_load(g_file_path, &gfile)) return 1;

    /* ---- allocate tx / z buffers ----
       demod_frame needs Nmf + M + Ncp = 1152 samples per call,
       so we pad by M+Ncp zeros at the end. */
    int total_tx = n_frames * Nmf + M + Ncp;
    RADE_COMP *tx  = (RADE_COMP *)calloc(total_tx, sizeof(RADE_COMP));
    float     *z_all = (float *)malloc((size_t)n_frames * z_per_frame * sizeof(float));
    assert(tx && z_all);

    /* ---- generate random QPSK symbols and modulate ---- */
    for (int fr = 0; fr < n_frames; fr++) {
        float *z = &z_all[fr * z_per_frame];
        for (int i = 0; i < z_per_frame; i++) z[i] = prbs_pm1();
        rade_ofdm_mod_frame(&ofdm, &tx[fr * Nmf], z);
    }

    /* ---- apply multipath (if g_file supplied) ---- */
    if (gfile.G) apply_multipath(tx, total_tx, &gfile);

    /* ---- add AWGN ---- */
    for (int n = 0; n < total_tx; n++) {
        tx[n].real += noise_std * randn();
        tx[n].imag += noise_std * randn();
    }

    /* ---- demodulate and count bit errors ---- */
    float *z_hat = (float *)malloc(z_per_frame * sizeof(float));
    assert(z_hat);

    long n_bits = 0, n_errors = 0;
    for (int fr = 0; fr < n_frames; fr++) {
        float snr_est = 0.0f;
        /* rx_in points to Nmf+M+Ncp samples; last frame padded with zeros */
        rade_ofdm_demod_frame(&ofdm, z_hat, &tx[fr * Nmf],
                              0,   /* time_offset: perfect timing */
                              0,   /* endofover: normal frame */
                              1,   /* coarse_mag: amplitude correction */
                              &snr_est);

        float *z = &z_all[fr * z_per_frame];
        for (int i = 0; i < z_per_frame; i++) {
            n_bits++;
            if ((z[i] > 0.0f) != (z_hat[i] > 0.0f)) n_errors++;
        }
    }

    float ber = (float)n_errors / (float)n_bits;
    printf("BER: %.4f  bits: %ld  errors: %ld\n", ber, n_bits, n_errors);

    free(tx); free(z_all); free(z_hat);
    if (gfile.G) free(gfile.G);
    return 0;
}
