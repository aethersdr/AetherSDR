/* See fftw_compat.h. Part of AetherSDR, GPL-3.0-or-later. */
#include "fftw_compat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct aether_fft_plan {
    int n, sign, real;
    void *in;
    fftwf_complex *out;
    double *tw; /* cos/sin pairs for the radix-2 path */
};

void *fftwf_malloc(size_t n) { return malloc(n); }
void fftwf_free(void *p) { free(p); }

static int is_pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

static fftwf_plan make(int n, void *in, fftwf_complex *out, int sign, int real)
{
    fftwf_plan p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->n = n;
    p->sign = sign;
    p->real = real;
    p->in = in;
    p->out = out;
    if (is_pow2(n)) {
        p->tw = malloc(sizeof(double) * (size_t)n);
        if (!p->tw) {
            free(p);
            return NULL;
        }
        for (int k = 0; k < n / 2; k++) {
            double a = sign * 2.0 * M_PI * k / n;
            p->tw[2 * k] = cos(a);
            p->tw[2 * k + 1] = sin(a);
        }
    }
    return p;
}

fftwf_plan fftwf_plan_dft_1d(int n, fftwf_complex *in, fftwf_complex *out, int sign, unsigned flags)
{
    (void)flags;
    return make(n, in, out, sign, 0);
}

fftwf_plan fftwf_plan_dft_r2c_1d(int n, float *in, fftwf_complex *out, unsigned flags)
{
    (void)flags;
    return make(n, in, out, FFTW_FORWARD, 1);
}

/* In-place iterative radix-2 over x (n complex doubles). */
static void radix2(double *x, int n, const double *tw)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            double t0 = x[2 * i], t1 = x[2 * i + 1];
            x[2 * i] = x[2 * j];
            x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = t0;
            x[2 * j + 1] = t1;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; k++) {
                double wr = tw[2 * k * step], wi = tw[2 * k * step + 1];
                double *a = x + 2 * (i + k), *b = x + 2 * (i + k + len / 2);
                double br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;
                b[0] = a[0] - br;
                b[1] = a[1] - bi;
                a[0] += br;
                a[1] += bi;
            }
        }
    }
}

void fftwf_execute(const fftwf_plan p)
{
    if (!p)
        return;
    int n = p->n;
    double *x = malloc(sizeof(double) * 2 * (size_t)n);
    if (!x)
        return;
    for (int i = 0; i < n; i++) {
        if (p->real) {
            x[2 * i] = ((float *)p->in)[i];
            x[2 * i + 1] = 0.0;
        } else {
            x[2 * i] = ((fftwf_complex *)p->in)[i][0];
            x[2 * i + 1] = ((fftwf_complex *)p->in)[i][1];
        }
    }
    int nout = p->real ? n / 2 + 1 : n;
    if (p->tw) {
        radix2(x, n, p->tw);
        for (int k = 0; k < nout; k++) {
            p->out[k][0] = (float)x[2 * k];
            p->out[k][1] = (float)x[2 * k + 1];
        }
    } else {
        for (int k = 0; k < nout; k++) {
            double sr = 0.0, si = 0.0;
            for (int i = 0; i < n; i++) {
                double a = p->sign * 2.0 * M_PI * (double)((long long)k * i % n) / n;
                double c = cos(a), s = sin(a);
                sr += x[2 * i] * c - x[2 * i + 1] * s;
                si += x[2 * i] * s + x[2 * i + 1] * c;
            }
            p->out[k][0] = (float)sr;
            p->out[k][1] = (float)si;
        }
    }
    free(x);
}

void fftwf_destroy_plan(fftwf_plan p)
{
    if (!p)
        return;
    free(p->tw);
    free(p);
}

int fftwf_import_wisdom_from_file(FILE *f)
{
    (void)f;
    return 1;
}

void fftwf_export_wisdom_to_file(FILE *f) { (void)f; }
