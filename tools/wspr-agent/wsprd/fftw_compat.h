/* The part of FFTW's single-precision API that wsprd.c uses, so the decoder
 * builds without FFTW. Part of AetherSDR, GPL-3.0-or-later.
 *
 * wsprd's decode path needs only a 512-point complex forward FFT; that and
 * any other power of two take a radix-2 path. The .wav reader's sizes are
 * not powers of two and fall back to a direct DFT. The agent never reads
 * .wav files, so that path only has to be correct, not fast. Wisdom is a
 * no-op. */
#ifndef AETHER_FFTW_COMPAT_H
#define AETHER_FFTW_COMPAT_H

#include <stdio.h>

typedef float fftwf_complex[2];
typedef struct aether_fft_plan *fftwf_plan;

#define FFTW_FORWARD (-1)
#define FFTW_BACKWARD (+1)
#define FFTW_MEASURE 0u
#define FFTW_PATIENT (1u << 5)
#define FFTW_EXHAUSTIVE (1u << 3)
#define FFTW_ESTIMATE (1u << 6)
#define FFTW_ESTIMATE_PATIENT (FFTW_ESTIMATE | FFTW_PATIENT)

void *fftwf_malloc(size_t n);
void fftwf_free(void *p);
fftwf_plan fftwf_plan_dft_1d(int n, fftwf_complex *in, fftwf_complex *out, int sign, unsigned flags);
fftwf_plan fftwf_plan_dft_r2c_1d(int n, float *in, fftwf_complex *out, unsigned flags);
void fftwf_execute(const fftwf_plan p);
void fftwf_destroy_plan(fftwf_plan p);
int fftwf_import_wisdom_from_file(FILE *f);
void fftwf_export_wisdom_to_file(FILE *f);

#endif
