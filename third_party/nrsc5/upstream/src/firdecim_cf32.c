#include "config.h"

#include <assert.h>
#include <stdint.h>

#include "firdecim_cf32.h"

#define WINDOW_SIZE 2048

struct firdecim_cf32 {
    float * taps;
    unsigned int ntaps;
    float complex * window;
    unsigned int idx;
};

firdecim_cf32 firdecim_cf32_create(const float * taps, unsigned int ntaps)
{
    firdecim_cf32 q;

    q = malloc(sizeof(*q));
    q->ntaps = (ntaps == 32) ? 32 : 15;
    q->taps = malloc(sizeof(float) * ntaps);
    q->window = calloc(WINDOW_SIZE, sizeof(float complex));
    firdecim_cf32_reset(q);

    // reverse order so we can push into the window
    for (unsigned int i = 0; i < ntaps; ++i)
    {
        q->taps[i] = taps[ntaps - 1 - i];
    }

    return q;
}

void firdecim_cf32_free(firdecim_cf32 q)
{
    free(q->taps);
    free(q->window);
    free(q);
}

void firdecim_cf32_reset(firdecim_cf32 q)
{
    q->idx = q->ntaps - 1;
}

static void push(firdecim_cf32 q, complex float x)
{
    if (q->idx == WINDOW_SIZE)
    {
        for (unsigned int i = 0; i < q->ntaps - 1; i++)
            q->window[i] = q->window[q->idx - q->ntaps + 1 + i];
        q->idx = q->ntaps - 1;
    }
    q->window[q->idx++] = x;
}

static float complex dotprod_32(const float complex *a, const float *b)
{
    float complex sum = { 0 };
    int i;

    for (i = 1; i < 16; i++)
    {
        sum += (a[i] + a[32-i]) * b[i];
    }
    sum += a[i] * b[i];

    return sum;
}

static float complex dotprod_halfband_4(const float complex *a, const float *b)
{
    float complex sum = { 0 };
    int i;

    for (i = 0; i < 7; i += 2)
    {
        sum += (a[i] + a[14-i]) * b[i];
    }
    sum += a[7];

    return sum;
}

void fir_cf32_execute(firdecim_cf32 q, const float complex *x, float complex *y)
{
    push(q, x[0]);
    *y = dotprod_32(&q->window[q->idx - q->ntaps], q->taps);
}

void halfband_cf32_execute(firdecim_cf32 q, const float complex *x, float complex *y)
{
    push(q, x[0]);
    *y = dotprod_halfband_4(&q->window[q->idx - q->ntaps], q->taps);
    push(q, x[1]);
}
