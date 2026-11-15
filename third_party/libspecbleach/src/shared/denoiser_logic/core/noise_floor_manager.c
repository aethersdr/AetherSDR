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

#include "shared/denoiser_logic/core/noise_floor_manager.h"
#include "shared/denoiser_logic/processing/spectral_whitening.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct NoiseFloorManager {
  SpectralWhitening* whitening;
  float* whitening_weights;
  uint32_t real_spectrum_size;
};

NoiseFloorManager* noise_floor_manager_initialize(const uint32_t fft_size,
                                                  const uint32_t sample_rate,
                                                  const uint32_t hop) {
  NoiseFloorManager* self =
      (NoiseFloorManager*)calloc(1U, sizeof(NoiseFloorManager));
  if (!self) {
    return NULL;
  }

  self->real_spectrum_size = (fft_size / 2U) + 1U;

  self->whitening = spectral_whitening_initialize(fft_size);
  if (!self->whitening) {
    free(self);
    return NULL;
  }

  self->whitening_weights =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->whitening_weights) {
    spectral_whitening_free(self->whitening);
    free(self);
    return NULL;
  }

  return self;
}

void noise_floor_manager_free(NoiseFloorManager* self) {
  if (!self) {
    return;
  }
  if (self->whitening) {
    spectral_whitening_free(self->whitening);
  }
  if (self->whitening_weights) {
    free(self->whitening_weights);
  }
  free(self);
}

void noise_floor_manager_apply(NoiseFloorManager* self,
                               uint32_t real_spectrum_size, uint32_t fft_size,
                               float* gain_spectrum, const float* noise_profile,
                               float reduction_amount,
                               float tonal_reduction_amount,
                               const float* tonal_mask,
                               float whitening_factor) {
  if (!self || !gain_spectrum || !noise_profile) {
    return;
  }

  if (reduction_amount >= 0.999f && tonal_reduction_amount >= 0.999f) {
    // Transparency Guard: If both reduction paths are at 0dB (1.0f),
    // we force unity gain to ensure BIT TRANSPARENCY and skip processing.
    for (uint32_t k = 0U; k < real_spectrum_size; k++) {
      gain_spectrum[k] = 1.0f;
    }
    for (uint32_t k = 1U; k < fft_size - k; k++) {
      gain_spectrum[fft_size - k] = gain_spectrum[k];
    }
    return;
  }

  // 1. Calculate whitening weights (including tapering)
  spectral_whitening_get_weights(self->whitening, whitening_factor,
                                 noise_profile, self->whitening_weights);

  // 2. Apply biasing + frequency-dependent floor
  for (uint32_t k = 0U; k < real_spectrum_size; k++) {
    float mask = (tonal_mask) ? tonal_mask[k] : 0.0f;
    // Proportional interpolation between regular reduction and tonal reduction
    // Note: These are already linear gains (0.0 to 1.0) passed from the loading
    // layer.
    float dual_path_reduction =
        (reduction_amount * (1.0f - mask)) + (tonal_reduction_amount * mask);

    /*
     * TONAL HAND-OFF:
     * As whitening increases, we want to smooth out the "notches" created by
     * tonal reduction to achieve a truly flat white residual.
     * We interpolate between the dual-path reduction (with notches) and the
     * broadband-only reduction level based on the whitening strength.
     */
    float target_reduction =
        dual_path_reduction +
        (whitening_factor * (reduction_amount - dual_path_reduction));

    /*
     * DYNAMIC WHITENING DEPTH & BIT TRANSPARENCY:
     * To achieve bit transparency at 0dB reduction, we damp the whitening
     * weights toward 1.0 as the target reduction level approaches 1.0 (no
     * reduction). This also makes the effect feel more natural and dynamic.
     */
    float reduction_depth = 1.0f - target_reduction;
    float effective_whitening_weight =
        1.0f + (reduction_depth * (self->whitening_weights[k] - 1.0f));

    float whitened_floor = target_reduction * effective_whitening_weight;

    // Safety clamp (Floor cannot exceed Unity Gain)
    if (whitened_floor > 1.0f) {
      whitened_floor = 1.0f;
    }

    /*
     * CLAMPING FLOOR:
     * We use fmaxf to ensure the gain never drops below the whitened floor.
     * This "replaces" the suppressed noise with the floor level,
     * effectively shaving off the spectral shape of the noise residual.
     */
    gain_spectrum[k] = fmaxf(whitened_floor, gain_spectrum[k]);
  }

  // 3. Symmetric copy
  for (uint32_t k = 1U; k < fft_size - k; k++) {
    gain_spectrum[fft_size - k] = gain_spectrum[k];
  }
}
