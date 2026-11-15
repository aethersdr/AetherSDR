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

#include "shared/denoiser_logic/processing/spectral_whitening.h"
#include "shared/configurations.h"
#include "shared/utils/spectral_utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int float_comparator(const void* a, const void* b) {
  float x = *(const float*)a;
  float y = *(const float*)b;
  return (x > y) - (x < y);
}

struct SpectralWhitening {
  float* tapering_window;
  uint32_t fft_size;
  uint32_t real_spectrum_size;
};

SpectralWhitening* spectral_whitening_initialize(const uint32_t fft_size) {
  SpectralWhitening* self =
      (SpectralWhitening*)calloc(1U, sizeof(SpectralWhitening));
  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;

  self->tapering_window =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->tapering_window) {
    free(self);
    return NULL;
  }

  // Tapering is disabled to allow true 100% flat whitening across the spectrum.
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    self->tapering_window[k] = 1.0f;
  }

  return self;
}

void spectral_whitening_free(SpectralWhitening* self) {
  if (!self) {
    return;
  }
  if (self->tapering_window) {
    free(self->tapering_window);
  }
  free(self);
}

void spectral_whitening_get_weights(SpectralWhitening* self,
                                    float whitening_factor,
                                    const float* noise_profile,
                                    float* weights_out) {
  if (!self || !weights_out || !noise_profile) {
    return;
  }

  // 1. Create a smoothed copy of the noise profile for stable weight
  // calculation
  float* smoothed_profile =
      (float*)malloc(self->real_spectrum_size * sizeof(float));
  if (!smoothed_profile) {
    return;
  }
  memcpy(smoothed_profile, noise_profile,
         self->real_spectrum_size * sizeof(float));
  smooth_spectrum(smoothed_profile, self->real_spectrum_size, 0.5f);

  // 2. Calculate Broadband Anchor Magnitude (using Median)
  // This prevents narrow tonal spikes from driving the whitening ceiling,
  // which causes hum leakage.
  float* sort_buffer = (float*)malloc(self->real_spectrum_size * sizeof(float));
  if (!sort_buffer) {
    free(smoothed_profile);
    return;
  }
  memcpy(sort_buffer, smoothed_profile,
         self->real_spectrum_size * sizeof(float));
  qsort(sort_buffer, self->real_spectrum_size, sizeof(float), float_comparator);

  float anchor_magnitude = sort_buffer[self->real_spectrum_size / 2];
  if (anchor_magnitude < SPECTRAL_EPSILON) {
    anchor_magnitude = SPECTRAL_EPSILON;
  }

  free(sort_buffer);

  // Use whitening_factor directly (already normalized 0.0-1.0 by the loader)
  float normalized_factor = whitening_factor;

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    float weight = 1.0f;
    if (normalized_factor > 0.0f && smoothed_profile[k] > SPECTRAL_EPSILON) {
      // Power-law valley filling anchored to Broadband Hiss level
      // This results in weights >= 1.0 for valleys and <= 1.0 for spikes
      weight = powf(anchor_magnitude / smoothed_profile[k], normalized_factor);
    }
    // Weights include unity-tapering (for now)
    weights_out[k] = weight * self->tapering_window[k];
  }

  free(smoothed_profile);
}
