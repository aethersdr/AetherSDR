/*---------------------------------------------------------------------------*\

  real2iq.c

  Converts real baseband signal to complex IQ using Hilbert transform.
  Reads float32 samples from stdin, writes complex float32 (I,Q) to stdout.

\*---------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* FIR Hilbert transformer coefficients (127 taps, odd length)
   h[n] = 2/(pi*n) for odd n, 0 for even n, windowed with Hamming */
#define HILBERT_NTAPS 127
#define HILBERT_DELAY ((HILBERT_NTAPS - 1) / 2)

static float hilbert_coeffs[HILBERT_NTAPS];

static void init_hilbert(void) {
    int center = HILBERT_DELAY;
    for (int i = 0; i < HILBERT_NTAPS; i++) {
        int n = i - center;
        if (n == 0) {
            hilbert_coeffs[i] = 0.0f;
        } else if (n % 2 == 0) {
            hilbert_coeffs[i] = 0.0f;
        } else {
            /* Hilbert: h[n] = 2/(pi*n) for odd n */
            float h = 2.0f / (M_PI * n);
            /* Hamming window */
            float w = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (HILBERT_NTAPS - 1));
            hilbert_coeffs[i] = h * w;
        }
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    init_hilbert();

    /* Read entire input into memory */
    size_t capacity = 1024 * 1024;  /* Start with 1M samples */
    size_t n_samples = 0;
    float *input = malloc(capacity * sizeof(float));
    if (!input) {
        fprintf(stderr, "real2iq: malloc failed\n");
        return 1;
    }

    size_t nread;
    while ((nread = fread(input + n_samples, sizeof(float), 4096, stdin)) > 0) {
        n_samples += nread;
        if (n_samples + 4096 > capacity) {
            capacity *= 2;
            input = realloc(input, capacity * sizeof(float));
            if (!input) {
                fprintf(stderr, "real2iq: realloc failed\n");
                return 1;
            }
        }
    }

    if (n_samples == 0) {
        fprintf(stderr, "real2iq: no input samples\n");
        free(input);
        return 1;
    }

    /* Allocate output buffer (complex = 2 floats per sample) */
    float *output = malloc(n_samples * 2 * sizeof(float));
    if (!output) {
        fprintf(stderr, "real2iq: malloc failed for output\n");
        free(input);
        return 1;
    }

    /* Apply Hilbert FIR filter to get Q (imaginary) component
       I (real) component is the delayed input */
    for (size_t i = 0; i < n_samples; i++) {
        /* Real part: delayed input */
        float real_part;
        if (i >= HILBERT_DELAY) {
            real_part = input[i - HILBERT_DELAY];
        } else {
            real_part = 0.0f;
        }

        /* Imaginary part: Hilbert filtered */
        float imag_part = 0.0f;
        for (int k = 0; k < HILBERT_NTAPS; k++) {
            int idx = (int)i - k;
            if (idx >= 0 && idx < (int)n_samples) {
                imag_part += hilbert_coeffs[k] * input[idx];
            }
        }

        output[i * 2]     = real_part;
        output[i * 2 + 1] = imag_part;
    }

    /* Write output */
    fwrite(output, sizeof(float), n_samples * 2, stdout);

    free(input);
    free(output);

    return 0;
}
