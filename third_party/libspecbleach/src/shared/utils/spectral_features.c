/*
libspecbleach - A spectral processing library

Copyright 2022 Luciano Dato <lucianodato@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "spectral_features.h"
#include <math.h>
#include <stdlib.h>

struct SpectralFeatures {
  float* power_spectrum;
  float* phase_spectrum;
  float* magnitude_spectrum;

  uint32_t real_spectrum_size;
};

SpectralFeatures* spectral_features_initialize(
    const uint32_t real_spectrum_size) {
  SpectralFeatures* self =
      (SpectralFeatures*)calloc(1U, sizeof(SpectralFeatures));
  if (!self) {
    return NULL;
  }

  self->real_spectrum_size = real_spectrum_size;

  self->power_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->phase_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->magnitude_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));

  if (!self->power_spectrum || !self->phase_spectrum ||
      !self->magnitude_spectrum) {
    spectral_features_free(self);
    return NULL;
  }

  return self;
}

void spectral_features_free(SpectralFeatures* self) {
  if (!self) {
    return;
  }
  free(self->power_spectrum);
  free(self->phase_spectrum);
  free(self->magnitude_spectrum);

  free(self);
}

float* get_power_spectrum(SpectralFeatures* self) {
  return self->power_spectrum;
}
float* get_magnitude_spectrum(SpectralFeatures* self) {
  return self->magnitude_spectrum;
}
float* get_phase_spectrum(SpectralFeatures* self) {
  return self->phase_spectrum;
}

static bool compute_power_spectrum(SpectralFeatures* self,
                                   const float* fft_spectrum,
                                   const uint32_t fft_spectrum_size) {
  if (!self || !fft_spectrum || !fft_spectrum_size) {
    return false;
  }

  const uint32_t n = fft_spectrum_size;
  const uint32_t n2 = n / 2U;
  const bool is_even = (n % 2U == 0);

  // DC bin
  self->power_spectrum[0] = fft_spectrum[0] * fft_spectrum[0];

  // Complex bins
  for (uint32_t k = 1U; k < n2; k++) {
    float real = fft_spectrum[k];
    float imag = fft_spectrum[n - k];
    self->power_spectrum[k] = (real * real) + (imag * imag);
  }

  // Nyquist bin
  if (is_even) {
    self->power_spectrum[n2] = fft_spectrum[n2] * fft_spectrum[n2];
  }

  return true;
}

static bool compute_magnitude_spectrum(SpectralFeatures* self,
                                       const float* fft_spectrum,
                                       const uint32_t fft_spectrum_size) {
  if (!self || !fft_spectrum || !fft_spectrum_size) {
    return false;
  }

  const uint32_t n = fft_spectrum_size;
  const uint32_t n2 = n / 2U;
  const bool is_even = (n % 2U == 0);

  // DC bin
  self->magnitude_spectrum[0] = fabsf(fft_spectrum[0]);

  // Complex bins
  for (uint32_t k = 1U; k < n2; k++) {
    self->magnitude_spectrum[k] = hypotf(fft_spectrum[k], fft_spectrum[n - k]);
  }

  // Nyquist bin
  if (is_even) {
    self->magnitude_spectrum[n2] = fabsf(fft_spectrum[n2]);
  }

  return true;
}

static bool compute_phase_spectrum(SpectralFeatures* self,
                                   const float* fft_spectrum,
                                   const uint32_t fft_spectrum_size) {
  if (!self || !fft_spectrum || !fft_spectrum_size) {
    return false;
  }

  const uint32_t n = fft_spectrum_size;
  const uint32_t n2 = n / 2U;
  const bool is_even = (n % 2U == 0);

  // DC bin - purely real
  self->phase_spectrum[0] = atan2f(0.F, fft_spectrum[0]);

  // Complex bins
  for (uint32_t k = 1U; k < n2; k++) {
    float real = fft_spectrum[k];
    float imag = fft_spectrum[n - k];
    self->phase_spectrum[k] = atan2f(imag, real);
  }

  // Nyquist bin - purely real
  if (is_even) {
    self->phase_spectrum[n2] = atan2f(0.F, fft_spectrum[n2]);
  }

  return true;
}

float* get_spectral_feature(SpectralFeatures* self, const float* fft_spectrum,
                            uint32_t fft_spectrum_size, SpectrumType type) {
  if (!self || !fft_spectrum || fft_spectrum_size == 0U) {
    return NULL;
  }

  switch (type) {
    case POWER_SPECTRUM:
      compute_power_spectrum(self, fft_spectrum, fft_spectrum_size);
      return get_power_spectrum(self);
      break;
    case MAGNITUDE_SPECTRUM:
      compute_magnitude_spectrum(self, fft_spectrum, fft_spectrum_size);
      return get_magnitude_spectrum(self);
      break;
    case PHASE_SPECTRUM:
      compute_phase_spectrum(self, fft_spectrum, fft_spectrum_size);
      return get_phase_spectrum(self);
      break;

    default:
      return NULL;
      break;
  }
}
