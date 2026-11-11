// Closed-loop simulation of the predistortion core, socket-free.
//
// Signal path per block, at the waveform's 24 kHz complex rate:
//   two-tone -> pd_core_correct -> radio TX filter model -> PA model
//            -> loop delay, NCO offset, gain, noise -> feedback
//   reference (what the waveform sent) + feedback -> pd_align -> pd_core_calibrate
//
// The reference is taken BEFORE the radio's TX filter, because that is all a
// waveform knows; the calibration has to cope with the filter between them.
// Prints IMD3/IMD5 of the PA output before and after correction.
//
//   pd_sim [--filter-hz F] [--offset-hz F] [--delay N] [--seconds S]
//          [--frac-delay F] [--realtime 0|1] [--expect-imd3-gain DB]
//          [--expect-imd3-gain-below DB]
// Exit status 0; 1 when an --expect-* bound is not met; 2 on a setup failure.
//
// Environment, for investigation:
//   PD_SIM_TRACE=1  print WDSP's calibration state twice a second
//   PD_SIM_AMPM=R   the PA's AM/PM at full drive, radians (default 0.25)
//   PD_SIM_IDEAL=1  replace WDSP's correction by the PA model's exact
//                   inverse, to show the harness's own measurement floor
#include "pd_align.h"
#include "pd_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 24000
#define BLOCK 1024
#define TONE_HZ 700.0      // tones at -700 and +700 Hz: IMD3 at +-2100, IMD5 at +-3500
#define TONE_AMP 0.4       // each tone; the two-tone peak is 0.8
#define FIR_TAPS 255

typedef struct { double i, q; } cpx;

// Memoryless PA: Rapp AM/AM (smoothness p) and Saleh-style AM/PM.
static cpx pa(cpx x)
{
    const double sat = 1.0, p = 2.0;
    static double am_pm = -1;
    if (am_pm < 0) am_pm = getenv("PD_SIM_AMPM") ? atof(getenv("PD_SIM_AMPM")) : 0.25;   // rad at full drive
    double a = hypot(x.i, x.q);
    if (a == 0)
        return x;
    double g = 1.0 / pow(1.0 + pow(a / sat, 2 * p), 1.0 / (2 * p));
    double ph = am_pm * a * a / (1.0 + a * a);
    double c = cos(ph), s = sin(ph);
    return (cpx){ g * (x.i * c - x.q * s), g * (x.i * s + x.q * c) };
}

// Linear-phase low-pass, passing -f..+f around the carrier.
static void design_lowpass(double* h, double f)
{
    double fc = f / RATE, sum = 0;
    for (int n = 0; n < FIR_TAPS; n++) {
        int m = n - (FIR_TAPS - 1) / 2;
        double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2 * M_PI * n / (FIR_TAPS - 1)) + 0.08 * cos(4 * M_PI * n / (FIR_TAPS - 1));
        h[n] = sinc * w;
        sum += h[n];
    }
    for (int n = 0; n < FIR_TAPS; n++)
        h[n] /= sum;
}

typedef struct { double h[FIR_TAPS]; cpx z[FIR_TAPS]; int pos; int on; } fir;

static cpx fir_step(fir* f, cpx x)
{
    if (!f->on)
        return x;
    f->z[f->pos] = x;
    cpx y = { 0, 0 };
    int k = f->pos;
    for (int n = 0; n < FIR_TAPS; n++) {
        y.i += f->h[n] * f->z[k].i;
        y.q += f->h[n] * f->z[k].q;
        k = k == 0 ? FIR_TAPS - 1 : k - 1;
    }
    f->pos = (f->pos + 1) % FIR_TAPS;
    return y;
}

// Power (dB) of the component at hz over n samples (n spans whole cycles).
static double tone_db(const cpx* x, int n, double hz)
{
    double re = 0, im = 0;
    for (int k = 0; k < n; k++) {
        double ph = -2 * M_PI * hz * k / RATE;
        re += x[k].i * cos(ph) - x[k].q * sin(ph);
        im += x[k].i * sin(ph) + x[k].q * cos(ph);
    }
    double a = hypot(re, im) / n;
    return 20 * log10(a + 1e-300);
}

typedef struct { double imd3, imd5, imd7; } imd;

static imd measure(const cpx* x, int n)
{
    double t = 0.5 * (tone_db(x, n, TONE_HZ) + tone_db(x, n, -TONE_HZ));
    imd r;
    r.imd3 = fmax(tone_db(x, n, 3 * TONE_HZ), tone_db(x, n, -3 * TONE_HZ)) - t;
    r.imd5 = fmax(tone_db(x, n, 5 * TONE_HZ), tone_db(x, n, -5 * TONE_HZ)) - t;
    r.imd7 = fmax(tone_db(x, n, 7 * TONE_HZ), tone_db(x, n, -7 * TONE_HZ)) - t;
    return r;
}

static double gauss(unsigned* s)
{
    // Box-Muller from a small LCG: deterministic across runs and platforms.
    double u1, u2;
    *s = *s * 1664525u + 1013904223u; u1 = ((*s >> 8) + 1.0) / 16777217.0;
    *s = *s * 1664525u + 1013904223u; u2 = ((*s >> 8) + 1.0) / 16777217.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

int main(int argc, char** argv)
{
    double filter_hz = 0, offset_hz = 0.05, seconds = 20, expect = -1, expect_below = -1;
    int delay = 37, max_delay = 400;
    double frac = 0;
    int realtime = 0;
    double fit_ms = 0;
    int fit_waits = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--filter-hz")) filter_hz = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--offset-hz")) offset_hz = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--delay")) delay = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--max-delay")) max_delay = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--frac-delay")) frac = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--expect-imd3-gain")) expect = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--expect-imd3-gain-below")) expect_below = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--realtime")) realtime = atoi(argv[i + 1]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    pd_core* core = pd_core_create(RATE, BLOCK, 2 * TONE_AMP);
    pd_align* al = pd_align_create(max_delay, max_delay + 4 * BLOCK + 512);
    if (!core || !al) { fprintf(stderr, "setup failed\n"); return 2; }

    // Fractional part of the loop delay: a 31-tap windowed-sinc delay of
    // 15 + frac samples on the feedback.
    fir fd = { .on = frac != 0 };
    if (fd.on) {
        double sum = 0;
        for (int n = 0; n < FIR_TAPS; n++) fd.h[n] = 0;
        for (int n = 0; n < 31; n++) {
            double m = n - 15 - frac;
            fd.h[n] = (m == 0 ? 1 : sin(M_PI * m) / (M_PI * m)) * (0.54 - 0.46 * cos(2 * M_PI * (n + 0.5) / 31.5));
            sum += fd.h[n];
        }
        for (int n = 0; n < 31; n++) fd.h[n] /= sum;
    }
    fir filt = { .on = filter_hz > 0 };
    if (filt.on) design_lowpass(filt.h, filter_hz);

    // The loop delay: the radio's filter plus the configured path delay.
    const int total_delay = delay + (filt.on ? (FIR_TAPS - 1) / 2 : 0);
    cpx* line = calloc((size_t)total_delay + 1, sizeof(cpx));
    int lpos = 0;
    unsigned seed = 12345;
    const double fb_gain = 0.7, noise = 1e-4;   // ~ -77 dB below full scale

    double tx[2 * BLOCK], rx[2 * BLOCK], atx[2 * BLOCK], arx[2 * BLOCK];
    cpx pa_out[BLOCK];
    const int win = RATE;                      // 1 s measurement window
    cpx* before = malloc(sizeof(cpx) * win);
    cpx* after = malloc(sizeof(cpx) * win);
    int nb = 0, na = 0;

    // Acquisition buffer for the delay estimate.
    const int acq = max_delay + 8192;
    double* acq_tx = malloc(sizeof(double) * 2 * acq);
    double* acq_rx = malloc(sizeof(double) * 2 * acq);
    int nacq = 0, aligned = 0;

    long long t = 0;
    double nco = 0, lp_i = 0, lp_q = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const long long total = (long long)(seconds * RATE);
    const long long after_start = total - win;
    pd_core_set_keyed(core, 1);

    while (t < total) {
        for (int k = 0; k < BLOCK; k++) {
            if (!aligned) {
                // Acquisition probe: a two-tone's envelope repeats every
                // 1/1400 s, which makes envelope correlation ambiguous.
                // Low-passed noise doesn't repeat, like speech.
                lp_i = 0.9 * lp_i + 0.1 * gauss(&seed);
                lp_q = 0.9 * lp_q + 0.1 * gauss(&seed);
                tx[2 * k] = 0.9 * lp_i;
                tx[2 * k + 1] = 0.9 * lp_q;
                continue;
            }
            double ph = 2 * M_PI * TONE_HZ * (double)(t + k) / RATE;
            tx[2 * k] = 2 * TONE_AMP * cos(ph);   // two tones at +-700 Hz: 2A cos(wt) on I
            tx[2 * k + 1] = 0;
        }
        if (getenv("PD_SIM_IDEAL") && aligned) {
            // Exact inverse of pa() toward a linear gain equal to its gain at
            // the two-tone peak: shows the measurement floor of this harness.
            const double peak = 2 * TONE_AMP;
            const double gt = hypot(pa((cpx){ peak, 0 }).i, pa((cpx){ peak, 0 }).q) / peak;
            for (int k = 0; k < BLOCK; k++) {
                double a = hypot(tx[2 * k], tx[2 * k + 1]);
                if (a == 0) continue;
                double want = gt * a, lo = 0, hi = 4;
                for (int it = 0; it < 60; it++) {
                    double mid = 0.5 * (lo + hi);
                    cpx y = pa((cpx){ mid, 0 });
                    if (hypot(y.i, y.q) < want) lo = mid; else hi = mid;
                }
                cpx y = pa((cpx){ lo, 0 });
                double ph = -atan2(y.q, y.i), g = lo / a;
                double i0 = tx[2 * k], q0 = tx[2 * k + 1];
                tx[2 * k] = g * (i0 * cos(ph) - q0 * sin(ph));
                tx[2 * k + 1] = g * (i0 * sin(ph) + q0 * cos(ph));
            }
        } else
            pd_core_correct(core, tx, BLOCK);      // tx is now the reference
        for (int k = 0; k < BLOCK; k++) {
            cpx d = fir_step(&filt, (cpx){ tx[2 * k], tx[2 * k + 1] });
            cpx y = pa(d);
            pa_out[k] = y;
            line[lpos] = y;
            cpx z = fir_step(&fd, line[(lpos + 1) % (total_delay + 1)]);
            lpos = (lpos + 1) % (total_delay + 1);
            double c = cos(nco), s = sin(nco);
            nco += 2 * M_PI * offset_hz / RATE;
            rx[2 * k] = fb_gain * (z.i * c - z.q * s) + noise * gauss(&seed);
            rx[2 * k + 1] = fb_gain * (z.i * s + z.q * c) + noise * gauss(&seed);
        }
        if (t >= after_start)
            for (int k = 0; k < BLOCK && na < win; k++) after[na++] = pa_out[k];

        if (!aligned) {
            int m = BLOCK < acq - nacq ? BLOCK : acq - nacq;
            memcpy(acq_tx + 2 * nacq, tx, sizeof(double) * 2 * m);
            memcpy(acq_rx + 2 * nacq, rx, sizeof(double) * 2 * m);
            nacq += m;
            if (nacq == acq) {
                double d = pd_align_estimate_delay(al, acq_tx, acq_rx, acq);
                printf("delay estimate %.2f (true %.2f)\n", d,
                       total_delay + (filt.on ? (FIR_TAPS - 1) / 2 : 0) + (fd.on ? 15 + frac : 0));
                if (d < 0) { fprintf(stderr, "no delay lock\n"); return 2; }
                pd_align_set_delay(al, d);
                aligned = 1;
            }
        }
        if (aligned) {
            int v = pd_align_push(al, tx, rx, BLOCK, atx, arx);
            pd_core_calibrate(core, atx, arx, v);
        }
        t += BLOCK;
        // WDSP fits on its own thread by the wall clock. --realtime 1 paces
        // the samples like the radio does; otherwise the simulation waits
        // only while a fit is running, as if the CPU were infinitely fast.
        if (!realtime) {
            extern void GetPSInfo(int, int*);
            int info[16];
            extern void pscc(int, int, double*, double*);
            double none[2] = { 0, 0 };
            struct timespec w0, w1;
            clock_gettime(CLOCK_MONOTONIC, &w0);
            int waited = 0;
            for (int w = 0; w < 5000; w++) {
                // An empty block steps WDSP's state machine, which only
                // notices a finished fit when pscc() runs.
                pscc(0, 0, none, none);
                GetPSInfo(0, info);
                // Wait while a fit computes (LCALC), but not once it has
                // finished and waits for the cross-fade, which needs us.
                if (info[15] != 6 || pd_core_swap_pending(core)) break;
                waited = 1;
                struct timespec d = { 0, 200000 };
                nanosleep(&d, NULL);
            }
            if (waited) {
                clock_gettime(CLOCK_MONOTONIC, &w1);
                fit_ms += (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) * 1e-6;
                fit_waits++;
            }
        }
        for (; realtime;) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) * 1e-9;
            double ahead = (double)t / RATE - el;
            if (ahead <= 0) break;
            struct timespec d = { 0, (long)(ahead * 1e9) };
            nanosleep(&d, NULL);
        }
        if (getenv("PD_SIM_TRACE") && t % (RATE / 2) < BLOCK) {
            int info[16];
            extern void GetPSInfo(int, int*);
            GetPSInfo(0, info);
            fprintf(stderr, "t=%5.2fs state=%d fits=%d correcting=%d info0=%x info6=%x off=%.3f\n",
                    (double)t / RATE, info[15], info[7], info[14], info[0], info[6], pd_align_offset_hz(al, RATE));
        }
    }

    // "Before": the same two-tone through the same filter and PA, uncorrected.
    fir f0 = { .on = filt.on };
    if (f0.on) memcpy(f0.h, filt.h, sizeof f0.h);
    for (int k = 0; k < win + FIR_TAPS; k++) {
        double ph = 2 * M_PI * TONE_HZ * (double)k / RATE;
        cpx y = pa(fir_step(&f0, (cpx){ 2 * TONE_AMP * cos(ph), 0 }));
        if (k >= FIR_TAPS) before[nb++] = y;
    }
    imd b = measure(before, nb), a = measure(after, na);
    printf("filter %s%.0f Hz, offset %.3f Hz (tracked %.3f), fits %d, correcting %d, flags 0x%x\n",
           filt.on ? "+-" : "none ", filter_hz, offset_hz, pd_align_offset_hz(al, RATE),
           pd_core_fits(core), pd_core_correcting(core), pd_core_fit_flags(core));
    if (fit_waits)
        printf("fit time: %.1f ms average over %d fits\n", fit_ms / fit_waits, fit_waits);
    printf("IMD3 %6.1f -> %6.1f dBc (%+.1f dB)\n", b.imd3, a.imd3, b.imd3 - a.imd3);
    printf("IMD5 %6.1f -> %6.1f dBc (%+.1f dB)\n", b.imd5, a.imd5, b.imd5 - a.imd5);
    printf("IMD7 %6.1f -> %6.1f dBc (%+.1f dB)\n", b.imd7, a.imd7, b.imd7 - a.imd7);

    int ok = (expect < 0 || (b.imd3 - a.imd3) >= expect)
          && (expect_below < 0 || (b.imd3 - a.imd3) < expect_below);
    pd_core_set_keyed(core, 0);
    pd_core_destroy(core);
    pd_align_destroy(al);
    return ok ? 0 : 1;
}
