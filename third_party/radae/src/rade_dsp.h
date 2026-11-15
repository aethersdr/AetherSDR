/*---------------------------------------------------------------------------*\

  rade_dsp.h

  DSP primitives and constants for RADAE C implementation.
  Complex arithmetic, matrix operations, and system constants.

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

#ifndef __RADE_DSP__
#define __RADE_DSP__

#include <math.h>

#ifndef __RADE_COMP__
#define __RADE_COMP__
typedef struct {
  float real;
  float imag;
} RADE_COMP;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                            SYSTEM CONSTANTS
\*---------------------------------------------------------------------------*/

/* Sample rates */
#define RADE_FS                 8000    /* Modem sample rate (Hz) */
#define RADE_FS_SPEECH          16000   /* Speech sample rate (Hz) */

/* OFDM parameters - derived from radae.py with pilots=True, cp=0.004 */
#define RADE_NC                 30      /* Number of OFDM carriers */
#define RADE_M                  160     /* Samples per OFDM symbol (Fs/Rs') */
#define RADE_NCP                32      /* Cyclic prefix samples (4ms * 8000) */
#define RADE_NS                 4       /* Data symbols per modem frame */
#define RADE_NZMF               3       /* Latent vectors per modem frame */

/* Derived constants */
#define RADE_NMF                ((RADE_NS+1)*(RADE_M+RADE_NCP))  /* Samples per modem frame = 960 */
#define RADE_NSYMB_MF           (RADE_NS+1)                      /* Total symbols per modem frame (data+pilot) */
#define RADE_NEOO               (RADE_NMF + RADE_M + RADE_NCP)   /* EOO frame samples = 1152 */

/* Neural network parameters */
#define RADE_LATENT_DIM         80      /* Latent vector dimension */
#define RADE_FRAMES_PER_STEP    4       /* Encoder/decoder stride */
#define RADE_NUM_FEATURES       20      /* Base vocoder features */
#define RADE_NUM_FEATURES_AUX   21      /* With auxiliary data */
#define RADE_NB_TOTAL_FEATURES  36      /* Total feature vector size (padded) */

/* BPF parameters */
#define RADE_BPF_NTAP           101     /* BPF filter taps */

/* Acquisition parameters */
#define RADE_ACQ_FRANGE         100.0f  /* Frequency search range (Hz) */
#define RADE_ACQ_FSTEP          2.5f    /* Frequency search step (Hz) */
#define RADE_ACQ_NFREQ          40      /* Number of frequency search steps */
#define RADE_ACQ_PACQ_ERR1      0.00001f /* Acquisition error probability 1 */
#define RADE_ACQ_PACQ_ERR2      0.0001f  /* Acquisition error probability 2 */

/* Receiver state machine */
#define RADE_STATE_SEARCH       0
#define RADE_STATE_CANDIDATE    1
#define RADE_STATE_SYNC         2

/* Timing constants */
#define RADE_TUNSYNC            3.0f    /* Time before losing sync (seconds) */
#define RADE_UW_ERROR_THRESH    7       /* Unique word error threshold */

/* Pilot symbols - Barker-13 code */
#define RADE_BARKER_LEN         13

/* Mathematical constants */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*---------------------------------------------------------------------------*\
                           COMPLEX ARITHMETIC
\*---------------------------------------------------------------------------*/

/* Complex multiplication: c = a * b */
static inline RADE_COMP rade_cmul(RADE_COMP a, RADE_COMP b) {
    RADE_COMP c;
    c.real = a.real * b.real - a.imag * b.imag;
    c.imag = a.real * b.imag + a.imag * b.real;
    return c;
}

/* Complex addition: c = a + b */
static inline RADE_COMP rade_cadd(RADE_COMP a, RADE_COMP b) {
    RADE_COMP c;
    c.real = a.real + b.real;
    c.imag = a.imag + b.imag;
    return c;
}

/* Complex subtraction: c = a - b */
static inline RADE_COMP rade_csub(RADE_COMP a, RADE_COMP b) {
    RADE_COMP c;
    c.real = a.real - b.real;
    c.imag = a.imag - b.imag;
    return c;
}

/* Complex conjugate: c = conj(a) */
static inline RADE_COMP rade_cconj(RADE_COMP a) {
    RADE_COMP c;
    c.real = a.real;
    c.imag = -a.imag;
    return c;
}

/* Complex magnitude: |a| */
static inline float rade_cabs(RADE_COMP a) {
    return sqrtf(a.real * a.real + a.imag * a.imag);
}

/* Complex magnitude squared: |a|^2 */
static inline float rade_cabs2(RADE_COMP a) {
    return a.real * a.real + a.imag * a.imag;
}

/* Complex phase angle: angle(a) */
static inline float rade_cangle(RADE_COMP a) {
    return atan2f(a.imag, a.real);
}

/* Complex from polar: a = r * exp(j*theta) */
static inline RADE_COMP rade_cpolar(float r, float theta) {
    RADE_COMP c;
    c.real = r * cosf(theta);
    c.imag = r * sinf(theta);
    return c;
}

/* Complex exponential: exp(j*theta) */
static inline RADE_COMP rade_cexp(float theta) {
    RADE_COMP c;
    c.real = cosf(theta);
    c.imag = sinf(theta);
    return c;
}

/* Complex scale: c = a * s (where s is real) */
static inline RADE_COMP rade_cscale(RADE_COMP a, float s) {
    RADE_COMP c;
    c.real = a.real * s;
    c.imag = a.imag * s;
    return c;
}

/* Complex division: c = a / b */
static inline RADE_COMP rade_cdiv(RADE_COMP a, RADE_COMP b) {
    float denom = b.real * b.real + b.imag * b.imag;
    RADE_COMP c;
    c.real = (a.real * b.real + a.imag * b.imag) / denom;
    c.imag = (a.imag * b.real - a.real * b.imag) / denom;
    return c;
}

/* Create complex number from real and imaginary parts */
static inline RADE_COMP rade_cmplx(float re, float im) {
    RADE_COMP c;
    c.real = re;
    c.imag = im;
    return c;
}

/* Zero complex number */
static inline RADE_COMP rade_czero(void) {
    RADE_COMP c = {0.0f, 0.0f};
    return c;
}

/* Unit complex number (1 + 0j) */
static inline RADE_COMP rade_cone(void) {
    RADE_COMP c = {1.0f, 0.0f};
    return c;
}

static inline RADE_COMP rade_cdot_comp(const RADE_COMP* a, const RADE_COMP* b, int n)
{
    RADE_COMP c = {0.f, 0.f};

    if (n == 1) 
    {
        c = rade_cmul(a[0], b[0]);
    }
    else if (n > 1)
    {
        int midpoint = floor(n / 2);
        RADE_COMP left = rade_cdot_comp(a, b, midpoint);
        RADE_COMP right = rade_cdot_comp(&a[midpoint], &b[midpoint], n - midpoint);
        c = rade_cadd(left, right);
    }
    return c;
}

static inline RADE_COMP rade_cdot_float(const RADE_COMP* a, const float* b, int n)
{
    RADE_COMP c = {0.f, 0.f};

    if (n == 1) 
    {
        c.real = a[0].real * b[0];
        c.imag = a[0].imag * b[0];
    }
    else if (n > 1)
    {
        int midpoint = floor(n / 2);
        RADE_COMP left = rade_cdot_float(a, b, midpoint);
        RADE_COMP right = rade_cdot_float(&a[midpoint], &b[midpoint], n - midpoint);
        c = rade_cadd(left, right);
    }
    return c;
}

/*---------------------------------------------------------------------------*\
                           VECTOR OPERATIONS
\*---------------------------------------------------------------------------*/

/* Complex dot product: sum(conj(a[i]) * b[i]) */
RADE_COMP rade_cdot(const RADE_COMP *a, const RADE_COMP *b, int n);

/* Complex matrix-vector multiply: y = A * x
   A is [rows x cols], x is [cols], y is [rows] */
void rade_cmvmul(RADE_COMP *y, const RADE_COMP *A, const RADE_COMP *x, int rows, int cols);

/* Complex matrix-vector multiply with real matrix: y = A * x
   A is [rows x cols] (real), x is [cols] (complex), y is [rows] (complex) */
void rade_cmvmul_real(RADE_COMP *y, const float *A, const RADE_COMP *x, int rows, int cols);

/*---------------------------------------------------------------------------*\
                           DSP UTILITIES
\*---------------------------------------------------------------------------*/

/* PA saturation model: tanh(|z|) * exp(j*angle(z)) */
static inline RADE_COMP rade_tanh_limit(RADE_COMP z) {
    float mag = rade_cabs(z);
    float angle = rade_cangle(z);
    float mag_limited = tanhf(mag);
    return rade_cpolar(mag_limited, angle);
}

/* Sinc function: sin(pi*x) / (pi*x) */
static inline float rade_sinc(float x) {
    if (fabsf(x) < 1e-10f) {
        return 1.0f;
    }
    float pix = M_PI * x;
    return sinf(pix) / pix;
}

/* Clamp value to range [min, max] */
static inline float rade_clampf(float x, float min, float max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

/* Linear interpolation */
static inline float rade_lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

/* Complex linear interpolation */
static inline RADE_COMP rade_clerp(RADE_COMP a, RADE_COMP b, float t) {
    RADE_COMP c;
    c.real = a.real + t * (b.real - a.real);
    c.imag = a.imag + t * (b.imag - a.imag);
    return c;
}

/*---------------------------------------------------------------------------*\
                           INITIALIZATION
\*---------------------------------------------------------------------------*/

/* Generate Barker-13 pilot symbols */
void rade_barker_pilots(RADE_COMP *P, int Nc);

/* Generate EOO pilot symbols (alternating sign) */
void rade_eoo_pilots(RADE_COMP *Pend, const RADE_COMP *P, int Nc);

#ifdef __cplusplus
}
#endif

#endif /* __RADE_DSP__ */
