/*---------------------------------------------------------------------------*\

  rade_dsp.c

  DSP primitives and utilities for RADAE C implementation.

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

#include "rade_dsp.h"
#include <string.h>

/*---------------------------------------------------------------------------*\
                           VECTOR OPERATIONS
\*---------------------------------------------------------------------------*/

/* Complex dot product: sum(conj(a[i]) * b[i]) */
RADE_COMP rade_cdot(const RADE_COMP *a, const RADE_COMP *b, int n) {
    RADE_COMP result = rade_czero();
    for (int i = 0; i < n; i++) {
        /* result += conj(a[i]) * b[i] */
        result.real += a[i].real * b[i].real + a[i].imag * b[i].imag;
        result.imag += a[i].real * b[i].imag - a[i].imag * b[i].real;
    }
    return result;
}

/* Complex matrix-vector multiply: y = A * x
   A is [rows x cols], x is [cols], y is [rows]
   Matrix A is stored row-major: A[row][col] = A[row*cols + col] */
void rade_cmvmul(RADE_COMP *y, const RADE_COMP *A, const RADE_COMP *x, int rows, int cols) {
    for (int r = 0; r < rows; r++) {
        RADE_COMP sum = rade_czero();
        const RADE_COMP *row = &A[r * cols];
        for (int c = 0; c < cols; c++) {
            sum = rade_cadd(sum, rade_cmul(row[c], x[c]));
        }
        y[r] = sum;
    }
}

/* Complex matrix-vector multiply with real matrix: y = A * x
   A is [rows x cols] (real), x is [cols] (complex), y is [rows] (complex)
   Matrix A is stored row-major */
void rade_cmvmul_real(RADE_COMP *y, const float *A, const RADE_COMP *x, int rows, int cols) {
    for (int r = 0; r < rows; r++) {
        RADE_COMP sum = rade_czero();
        const float *row = &A[r * cols];
        for (int c = 0; c < cols; c++) {
            sum.real += row[c] * x[c].real;
            sum.imag += row[c] * x[c].imag;
        }
        y[r] = sum;
    }
}

/*---------------------------------------------------------------------------*\
                           PILOT GENERATION
\*---------------------------------------------------------------------------*/

/* Barker-13 code */
static const float barker13[RADE_BARKER_LEN] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f
};

/* Generate Barker-13 pilot symbols for Nc carriers
   Pilots are real-valued, scaled by sqrt(2) for QPSK power normalization */
void rade_barker_pilots(RADE_COMP *P, int Nc) {
    float scale = sqrtf(2.0f);
    for (int i = 0; i < Nc; i++) {
        P[i].real = scale * barker13[i % RADE_BARKER_LEN];
        P[i].imag = 0.0f;
    }
}

/* Generate EOO pilot symbols (alternating sign on odd carriers) */
void rade_eoo_pilots(RADE_COMP *Pend, const RADE_COMP *P, int Nc) {
    for (int i = 0; i < Nc; i++) {
        Pend[i] = P[i];
        if (i % 2 == 1) {
            Pend[i].real = -Pend[i].real;
            Pend[i].imag = -Pend[i].imag;
        }
    }
}
