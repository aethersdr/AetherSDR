/* AetherSDR native Windows adapter; GPL-3.0-or-later.
 * clang-cl implements C complex arithmetic, while the Microsoft CRT exposes
 * complex math through _Fcomplex. Keep the pinned decoder's arithmetic intact
 * and bridge only the six CRT functions it uses. This header is private to the
 * nrsc5 C target; no C complex values cross its public float-array API.
 */
#pragma once

#if !defined(_WIN32) || !defined(__clang__)
#error "This adapter requires clang-cl targeting the Windows MSVC ABI"
#endif

/* Read math.h first: its legacy `complex` alias must not overwrite ours on a
 * later upstream include. UCRT also declares normf; the decoder has its own. */
#include <math.h>
#include_next <complex.h>

#undef complex
#define complex _Complex
#undef I
#define I (__builtin_complex(0.0f, 1.0f))
#undef _Complex_I
#define _Complex_I I
#ifndef CMPLXF
#define CMPLXF(real, imag) __builtin_complex((float)(real), (float)(imag))
#endif

static inline _Fcomplex aether_nrsc5_to_crt(float complex value)
{
    return _FCbuild(__real__(value), __imag__(value));
}

static inline float aether_nrsc5_crealf(float complex value)
{
    return __real__(value);
}

static inline float aether_nrsc5_cimagf(float complex value)
{
    return __imag__(value);
}

static inline float aether_nrsc5_cabsf(float complex value)
{
    return cabsf(aether_nrsc5_to_crt(value));
}

static inline float aether_nrsc5_cargf(float complex value)
{
    return cargf(aether_nrsc5_to_crt(value));
}

static inline float complex aether_nrsc5_conjf(float complex value)
{
    return CMPLXF(__real__(value), -__imag__(value));
}

static inline float complex aether_nrsc5_cexpf(float complex value)
{
    const _Fcomplex result = cexpf(aether_nrsc5_to_crt(value));
    return CMPLXF(crealf(result), cimagf(result));
}

#define crealf aether_nrsc5_crealf
#define cimagf aether_nrsc5_cimagf
#define cabsf aether_nrsc5_cabsf
#define cargf aether_nrsc5_cargf
#define conjf aether_nrsc5_conjf
#define cexpf aether_nrsc5_cexpf
#define normf aether_nrsc5_normf
