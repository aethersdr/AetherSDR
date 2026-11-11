// Replays aligned reference/feedback pairs captured with AETHER_PD_DUMP
// (float32 tx_i, tx_q, rx_i, rx_q at 24 kHz) through the calibration, so a
// bench capture can be tried against calibration changes without keying.
//   pd_replay FILE [--skip SECS] [--eq TAPS] [--train SECS] [--peak P]
//             [--loops N] [--pace-us US]
#include "pd_core.h"
#include "pd_eq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE 24000
#define BLOCK 512

void GetPSInfo(int channel, int* info);

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: pd_replay FILE [--skip S] [--eq TAPS] [--train S] [--peak P]\n");
        return 2;
    }
    double skip = 0, train = 1.0, peak = 0.7;
    int taps = 0, loops = 1, pace_us = 1000;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--skip")) skip = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--eq")) taps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--train")) train = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--peak")) peak = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--loops")) loops = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--pace-us")) pace_us = atoi(argv[i + 1]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 16;
    fseek(f, 0, SEEK_SET);
    float* raw = malloc(sizeof(float) * 4 * (size_t)n);
    double* tx = malloc(sizeof(double) * 2 * (size_t)n);
    double* rx = malloc(sizeof(double) * 2 * (size_t)n);
    if (fread(raw, 16, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    for (long k = 0; k < n; k++) {
        tx[2 * k] = raw[4 * k]; tx[2 * k + 1] = raw[4 * k + 1];
        rx[2 * k] = raw[4 * k + 2]; rx[2 * k + 1] = raw[4 * k + 3];
    }
    long start = (long)(skip * RATE);
    pd_eq* eq = NULL;
    if (taps > 0) {
        eq = pd_eq_create(taps);
        long m = (long)(train * RATE);
        pd_eq_train(eq, tx + 2 * start, rx + 2 * start, (int)m);
        if (!pd_eq_solve(eq)) { fprintf(stderr, "eq solve failed\n"); return 1; }
        printf("eq %d taps on %.1f s: residual %.2e (%.1f dB)\n", taps, train,
               pd_eq_residual(eq), 10 * __builtin_log10(pd_eq_residual(eq) + 1e-30));
    }
    pd_core* c = pd_core_create(RATE, BLOCK, peak);
    pd_core_set_keyed(c, 1);
    double etx[2 * BLOCK], erx[2 * BLOCK];
    int last_fits = -1;
    for (int lp = 0; lp < loops; lp++)
    for (long s = start; s + BLOCK <= n; s += BLOCK) {
        const double* a = tx + 2 * s;
        const double* b = rx + 2 * s;
        int v = BLOCK;
        if (eq) {
            v = pd_eq_apply(eq, a, b, BLOCK, etx, erx);
            a = etx; b = erx;
        }
        if (v > 0)
            pd_core_calibrate(c, a, b, v);
        // The corrector runs every block, as in the waveform: an accepted fit
        // fades in through xiqc, and calcc waits for that before going on.
        double sig[2 * BLOCK];
        memcpy(sig, tx + 2 * s, sizeof sig);
        pd_core_correct(c, sig, BLOCK);
        usleep((useconds_t)pace_us);   // let calcc's worker run
        if (getenv("PD_REPLAY_TRACE") && (s - start) % (RATE / BLOCK * BLOCK) < BLOCK) {
            int inf[16];
            GetPSInfo(0, inf);
            printf("trace %d %.2f:", lp, (double)s / RATE);
            for (int q = 0; q < 16; q++) printf(" %d", inf[q]);
            printf("\n");
        }
        if (pd_core_fits(c) != last_fits) {
            last_fits = pd_core_fits(c);
            printf("loop %d %.2f s: fits %d correcting %d state %d flags 0x%x\n", lp, (double)s / RATE,
                   last_fits, pd_core_correcting(c), pd_core_state(c), pd_core_fit_flags(c));
        }
    }
    usleep(500000);
    printf("end: fits %d correcting %d state %d flags 0x%x\n", pd_core_fits(c),
           pd_core_correcting(c), pd_core_state(c), pd_core_fit_flags(c));
    pd_core_destroy(c);
    pd_eq_destroy(eq);
    return 0;
}
