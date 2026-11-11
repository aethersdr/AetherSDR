#include "pd_eq.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Ridge added to the normal equations, relative to their mean diagonal: keeps
// the solve stable where the training signal has little energy.
#define PD_EQ_RIDGE 1e-6

struct pd_eq {
    int taps, centre;
    double complex* A;     // taps x taps: sum of conj(u) u^T
    double complex* b;     // taps: sum of conj(u) d
    double complex* h;     // the FIR, scaled to the reference's level
    double complex* hist;  // reference history for training/applying, newest first
    double complex* rxd;   // feedback delay line, length centre + 1
    double ptx, prx;       // training powers
    double resid;
    long trained;
    int filled, ready;
};

pd_eq* pd_eq_create(int taps)
{
    if (taps < 1 || !(taps & 1))
        return NULL;
    pd_eq* e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->taps = taps;
    e->centre = taps / 2;
    e->A = calloc((size_t)taps * taps, sizeof *e->A);
    e->b = calloc((size_t)taps, sizeof *e->b);
    e->h = calloc((size_t)taps, sizeof *e->h);
    e->hist = calloc((size_t)taps, sizeof *e->hist);
    e->rxd = calloc((size_t)e->centre + 1, sizeof *e->rxd);
    if (!e->A || !e->b || !e->h || !e->hist || !e->rxd) {
        pd_eq_destroy(e);
        return NULL;
    }
    return e;
}

void pd_eq_destroy(pd_eq* e)
{
    if (!e)
        return;
    free(e->A);
    free(e->b);
    free(e->h);
    free(e->hist);
    free(e->rxd);
    free(e);
}

void pd_eq_reset(pd_eq* e)
{
    memset(e->A, 0, sizeof *e->A * (size_t)e->taps * e->taps);
    memset(e->b, 0, sizeof *e->b * (size_t)e->taps);
    memset(e->h, 0, sizeof *e->h * (size_t)e->taps);
    memset(e->hist, 0, sizeof *e->hist * (size_t)e->taps);
    memset(e->rxd, 0, sizeof *e->rxd * (size_t)(e->centre + 1));
    e->ptx = e->prx = e->resid = 0;
    e->trained = 0;
    e->filled = 0;
    e->ready = 0;
}

static void push_hist(pd_eq* e, double complex x)
{
    memmove(e->hist + 1, e->hist, sizeof *e->hist * (size_t)(e->taps - 1));
    e->hist[0] = x;
}

static void push_rx(pd_eq* e, double complex x)
{
    memmove(e->rxd + 1, e->rxd, sizeof *e->rxd * (size_t)e->centre);
    e->rxd[0] = x;
}

void pd_eq_train(pd_eq* e, const double* tx, const double* rx, int n)
{
    const int L = e->taps;
    for (int i = 0; i < n; i++) {
        push_hist(e, tx[2 * i] + I * tx[2 * i + 1]);
        push_rx(e, rx[2 * i] + I * rx[2 * i + 1]);
        if (e->filled < L) {
            e->filled++;
            continue;
        }
        // Model: rx[t - centre] = sum_k h[k] tx[t - k].
        double complex d = e->rxd[e->centre];
        for (int j = 0; j < L; j++) {
            double complex cu = conj(e->hist[j]);
            e->b[j] += cu * d;
            for (int k = 0; k < L; k++)
                e->A[j * L + k] += cu * e->hist[k];
        }
        e->ptx += creal(e->hist[e->centre] * conj(e->hist[e->centre]));
        e->prx += creal(d * conj(d));
        e->trained++;
    }
}

long pd_eq_trained(const pd_eq* e) { return e->trained; }
int pd_eq_ready(const pd_eq* e) { return e->ready; }
double pd_eq_residual(const pd_eq* e) { return e->resid; }

int pd_eq_solve(pd_eq* e)
{
    const int L = e->taps;
    if (e->trained < 4 * L || !(e->ptx > 0) || !(e->prx > 0))
        return 0;
    double complex* M = malloc(sizeof *M * (size_t)L * (L + 1));
    if (!M)
        return 0;
    double diag = 0;
    for (int j = 0; j < L; j++)
        diag += creal(e->A[j * L + j]);
    const double ridge = PD_EQ_RIDGE * diag / L;
    for (int j = 0; j < L; j++) {
        for (int k = 0; k < L; k++)
            M[j * (L + 1) + k] = e->A[j * L + k] + (j == k ? ridge : 0);
        M[j * (L + 1) + L] = e->b[j];
    }
    // Gaussian elimination with partial pivoting.
    for (int c = 0; c < L; c++) {
        int p = c;
        for (int r = c + 1; r < L; r++)
            if (cabs(M[r * (L + 1) + c]) > cabs(M[p * (L + 1) + c]))
                p = r;
        if (cabs(M[p * (L + 1) + c]) < 1e-300) {
            free(M);
            return 0;
        }
        if (p != c)
            for (int k = c; k <= L; k++) {
                double complex t = M[c * (L + 1) + k];
                M[c * (L + 1) + k] = M[p * (L + 1) + k];
                M[p * (L + 1) + k] = t;
            }
        for (int r = c + 1; r < L; r++) {
            double complex f = M[r * (L + 1) + c] / M[c * (L + 1) + c];
            for (int k = c; k <= L; k++)
                M[r * (L + 1) + k] -= f * M[c * (L + 1) + k];
        }
    }
    for (int r = L - 1; r >= 0; r--) {
        double complex s = M[r * (L + 1) + L];
        for (int k = r + 1; k < L; k++)
            s -= M[r * (L + 1) + k] * e->h[k];
        e->h[r] = s / M[r * (L + 1) + r];
    }
    free(M);
    // Power of the fitted output, h^T conj(A) conj(h), and what it leaves.
    double py = 0, cross = 0;
    for (int j = 0; j < L; j++)
        for (int k = 0; k < L; k++)
            py += creal(e->h[j] * conj(e->h[k]) * conj(e->A[j * L + k]));
    for (int j = 0; j < L; j++)
        cross += creal(e->h[j] * conj(e->b[j]));
    e->resid = (e->prx - 2 * cross + py) / e->prx;
    if (e->resid < 0)
        e->resid = 0;
    // Keep the reference at its own level: WDSP's full scale is set by it.
    if (!(py > 0))
        return 0;
    const double g = sqrt(e->ptx / py);
    for (int j = 0; j < L; j++)
        e->h[j] *= g;
    memset(e->hist, 0, sizeof *e->hist * (size_t)L);
    memset(e->rxd, 0, sizeof *e->rxd * (size_t)(e->centre + 1));
    e->filled = 0;
    e->ready = 1;
    return 1;
}

int pd_eq_apply(pd_eq* e, const double* tx, const double* rx, int n, double* tx_out, double* rx_out)
{
    int valid = 0;
    for (int i = 0; i < n; i++) {
        push_hist(e, tx[2 * i] + I * tx[2 * i + 1]);
        push_rx(e, rx[2 * i] + I * rx[2 * i + 1]);
        if (e->filled < e->taps) {
            e->filled++;
            continue;
        }
        double complex y = 0;
        for (int k = 0; k < e->taps; k++)
            y += e->h[k] * e->hist[k];
        tx_out[2 * valid] = creal(y);
        tx_out[2 * valid + 1] = cimag(y);
        rx_out[2 * valid] = creal(e->rxd[e->centre]);
        rx_out[2 * valid + 1] = cimag(e->rxd[e->centre]);
        valid++;
    }
    return valid;
}
