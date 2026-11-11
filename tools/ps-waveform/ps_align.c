#include "ps_align.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Per-block loop gains of the phase tracker. With ~1024-sample blocks at
// 24 kHz this is a loop of about 1 Hz bandwidth: fast enough to follow the
// millihertz-to-hertz offsets two NCOs can have, slow enough that the
// amplifier's AM/PM (which moves at the envelope rate, hundreds of hertz and
// up) averages out inside each block instead of being tracked away.
#define PS_ALIGN_KP 0.3
#define PS_ALIGN_KI 0.05
// A delay estimate is trusted only above this normalized correlation.
#define PS_ALIGN_MIN_CORR 0.6
// Fractional-delay interpolator: windowed sinc, PS_ALIGN_TAPS taps centred
// on the wanted instant. 16 taps keep the error well below the feedback's
// noise for signals inside +-0.4 of the sample rate.
#define PS_ALIGN_TAPS 16
// Fine delay search step, in samples.
#define PS_ALIGN_FINE_STEP 0.01

struct ps_align {
    int max_delay;
    int history;      // ring length, in complex samples
    double* ring;     // reference history, interleaved I/Q
    long long count;  // reference samples written since the last reset
    int delay;        // whole samples
    double frac;      // 0 <= frac < 1, extra fractional samples
    double taps[PS_ALIGN_TAPS];
    double phase;     // feedback de-rotation, radians
    double step;      // de-rotation per sample, radians
};

// Taps that read x(t0 - frac) from x[t0 - 7 .. t0 + 8] (tap k reads t0 - 7 + k).
static void frac_taps(double frac, double* h)
{
    double sum = 0;
    for (int k = 0; k < PS_ALIGN_TAPS; k++) {
        double m = (k - (PS_ALIGN_TAPS / 2 - 1)) + frac;   // offset from the wanted instant
        double s = fabs(m) < 1e-12 ? 1.0 : sin(M_PI * m) / (M_PI * m);
        double w = 0.5 + 0.5 * cos(M_PI * m / (PS_ALIGN_TAPS / 2 + 1));
        h[k] = s * w;
        sum += h[k];
    }
    for (int k = 0; k < PS_ALIGN_TAPS; k++)
        h[k] /= sum;
}

ps_align* ps_align_create(int max_delay, int history)
{
    if (max_delay < 0 || history <= max_delay)
        return NULL;
    ps_align* a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->ring = calloc((size_t)history * 2, sizeof(double));
    if (!a->ring) {
        free(a);
        return NULL;
    }
    a->max_delay = max_delay;
    a->history = history;
    return a;
}

void ps_align_destroy(ps_align* a)
{
    if (!a)
        return;
    free(a->ring);
    free(a);
}

static double env_corr(const double* etx, const double* erx, int span, double lag, int n,
                       const double* txiq, double mtx)
{
    // Correlation of the feedback envelope with the reference envelope
    // delayed by a fractional lag: rx[i + lag] against tx[i].
    int whole = (int)floor(lag);
    double frac = lag - whole, h[PS_ALIGN_TAPS];
    frac_taps(frac, h);
    double s = 0, ptx = 0, prx = 0;
    for (int i = PS_ALIGN_TAPS; i < span - PS_ALIGN_TAPS; i++) {
        // reference delayed by frac at instant i: taps around i
        double ri = 0, rq = 0;
        for (int k = 0; k < PS_ALIGN_TAPS; k++) {
            int j = i - (PS_ALIGN_TAPS / 2 - 1) + k;
            ri += h[k] * txiq[2 * j];
            rq += h[k] * txiq[2 * j + 1];
        }
        double e = hypot(ri, rq) - mtx;
        int r = i + whole;
        if (r >= n)
            break;
        s += e * erx[r];
        ptx += e * e;
        prx += erx[r] * erx[r];
    }
    (void)etx;
    return (ptx > 0 && prx > 0) ? s / sqrt(ptx * prx) : 0;
}

double ps_align_estimate_delay(const ps_align* a, const double* tx, const double* rx, int n)
{
    if (n <= 2 * a->max_delay + 4 * PS_ALIGN_TAPS)
        return -1;
    double* etx = malloc(sizeof(double) * (size_t)n);
    double* erx = malloc(sizeof(double) * (size_t)n);
    if (!etx || !erx) {
        free(etx);
        free(erx);
        return -1;
    }
    double mtx = 0, mrx = 0;
    for (int i = 0; i < n; i++) {
        etx[i] = hypot(tx[2 * i], tx[2 * i + 1]);
        erx[i] = hypot(rx[2 * i], rx[2 * i + 1]);
        mtx += etx[i];
        mrx += erx[i];
    }
    mtx /= n;
    mrx /= n;
    for (int i = 0; i < n; i++) {
        etx[i] -= mtx;
        erx[i] -= mrx;
    }
    // Envelopes are immune to the NCO offset, so the search can't be fooled
    // by the feedback rotating against the reference.
    int best = -1;
    double bestr = 0;
    const int span = n - a->max_delay;
    for (int lag = 0; lag <= a->max_delay; lag++) {
        double s = 0, ptx = 0, prx = 0;
        for (int i = 0; i < span; i++) {
            s += etx[i] * erx[i + lag];
            ptx += etx[i] * etx[i];
            prx += erx[i + lag] * erx[i + lag];
        }
        double r = (ptx > 0 && prx > 0) ? s / sqrt(ptx * prx) : 0;
        if (r > bestr) {
            bestr = r;
            best = lag;
        }
    }
    double result = -1;
    if (bestr >= PS_ALIGN_MIN_CORR) {
        result = best;
        double fbest = bestr;
        for (double lag = best - 1.0; lag <= best + 1.0 + 1e-9; lag += PS_ALIGN_FINE_STEP) {
            if (lag < 0)
                continue;
            double r = env_corr(etx, erx, span, lag, n, tx, mtx);
            if (r > fbest) {
                fbest = r;
                result = lag;
            }
        }
    }
    free(etx);
    free(erx);
    return result;
}

void ps_align_set_delay(ps_align* a, double delay)
{
    if (!(delay >= 0))
        delay = 0;
    if (delay > a->max_delay)
        delay = a->max_delay;
    if (delay < PS_ALIGN_MIN_DELAY)
        delay = floor(delay + 0.5);
    a->delay = (int)floor(delay);
    a->frac = delay - a->delay;
    frac_taps(a->frac, a->taps);
}

double ps_align_delay(const ps_align* a) { return a->delay + a->frac; }

double ps_align_offset_hz(const ps_align* a, double rate)
{
    return a->step * rate / (2.0 * M_PI);
}

int ps_align_push(ps_align* a, const double* tx, const double* rx, int n,
                  double* tx_out, double* rx_out)
{
    if (n <= 0 || n + a->delay >= a->history)
        return 0;
    int valid = 0;
    double zr = 0, zi = 0;
    for (int i = 0; i < n; i++) {
        // Store the new reference sample first: with zero delay the pair is
        // the sample that just arrived.
        long long t = a->count + i;
        int w = (int)(t % a->history);
        a->ring[2 * w] = tx[2 * i];
        a->ring[2 * w + 1] = tx[2 * i + 1];

        long long src = t - a->delay;
        if (src < 0)
            continue;
        double ti, tq;
        if (a->frac == 0) {
            int r = (int)(src % a->history);
            ti = a->ring[2 * r];
            tq = a->ring[2 * r + 1];
        } else {
            // reference at src - frac, from ring[src - 7 .. src + 8]
            long long first = src - (PS_ALIGN_TAPS / 2 - 1);
            if (first < 0 || src + PS_ALIGN_TAPS / 2 > t)
                continue;
            ti = tq = 0;
            for (int k = 0; k < PS_ALIGN_TAPS; k++) {
                int r = (int)((first + k) % a->history);
                ti += a->taps[k] * a->ring[2 * r];
                tq += a->taps[k] * a->ring[2 * r + 1];
            }
        }
        double c = cos(a->phase), s = sin(a->phase);
        double ri = rx[2 * i] * c + rx[2 * i + 1] * s;
        double rq = rx[2 * i + 1] * c - rx[2 * i] * s;
        a->phase += a->step;
        tx_out[2 * valid] = ti;
        tx_out[2 * valid + 1] = tq;
        rx_out[2 * valid] = ri;
        rx_out[2 * valid + 1] = rq;
        valid++;
        // z = sum(rx * conj(tx)): its angle is the residual phase of the
        // de-rotated feedback against the reference.
        zr += ri * ti + rq * tq;
        zi += rq * ti - ri * tq;
    }
    a->count += n;
    if (valid > 0 && (zr != 0 || zi != 0)) {
        double err = atan2(zi, zr);
        a->phase += PS_ALIGN_KP * err;
        a->step += PS_ALIGN_KI * err / valid;
    }
    a->phase = fmod(a->phase, 2.0 * M_PI);
    return valid;
}
