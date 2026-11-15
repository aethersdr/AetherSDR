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

#include "shared/denoiser_logic/estimators/martin_noise_estimator.h"
#include "shared/configurations.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct MartinNoiseEstimator {
  uint32_t noise_spectrum_size;

  float* smoothed_psd;       // Smoothed power spectral density
  float* current_subwin_min; // Minimum observed in the current sub-window
  float* subwin_history; // History of minimums from last MARTIN_SUBWIN_COUNT
                         // sub-windows

  uint32_t frame_count;  // Current frame index within the sub-window
  uint32_t subwin_index; // Current sub-window index in history

  bool is_first_frame;
};

MartinNoiseEstimator* martin_noise_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size) {
  (void)sample_rate;
  (void)fft_size;

  MartinNoiseEstimator* self =
      (MartinNoiseEstimator*)calloc(1U, sizeof(MartinNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->noise_spectrum_size = noise_spectrum_size;
  self->smoothed_psd = (float*)calloc(noise_spectrum_size, sizeof(float));
  self->current_subwin_min = (float*)calloc(noise_spectrum_size, sizeof(float));
  self->subwin_history = (float*)calloc(
      (size_t)noise_spectrum_size * MARTIN_SUBWIN_COUNT, sizeof(float));

  if (!self->smoothed_psd || !self->current_subwin_min ||
      !self->subwin_history) {
    martin_noise_estimator_free(self);
    return NULL;
  }

  self->is_first_frame = true;
  self->frame_count = 0;
  self->subwin_index = 0;

  return self;
}

void martin_noise_estimator_free(MartinNoiseEstimator* self) {
  if (!self) {
    return;
  }
  free(self->smoothed_psd);
  free(self->current_subwin_min);
  free(self->subwin_history);
  free(self);
}

bool martin_noise_estimator_run(MartinNoiseEstimator* self,
                                const float* spectrum, float* noise_spectrum) {
  if (!self || !spectrum || !noise_spectrum) {
    return false;
  }

  float frame_energy = 0.F;
  for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
    frame_energy += spectrum[k];
  }
  frame_energy /= (float)self->noise_spectrum_size;

  if (self->is_first_frame) {
    if (frame_energy < ESTIMATOR_SILENCE_THRESHOLD) {
      memset(noise_spectrum, 0, self->noise_spectrum_size * sizeof(float));
      return true;
    }
    for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
      float val = spectrum[k] / MARTIN_BIAS_CORR;
      self->smoothed_psd[k] = val;
      self->current_subwin_min[k] = val;
      // Initialize history with current value
      for (uint32_t d = 0; d < MARTIN_SUBWIN_COUNT; d++) {
        self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) + d] = val;
      }
      noise_spectrum[k] = spectrum[k];
    }
    self->is_first_frame = false;
    self->frame_count = 1;
    return true;
  }

  // Silence check for subsequent frames
  if (frame_energy < ESTIMATOR_SILENCE_THRESHOLD) {
    // Return existing estimate without updating internal state
    goto calculate_output;
  }

  // 1. Update smoothed PSD
  for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
    self->smoothed_psd[k] = (MARTIN_SMOOTH_ALPHA * self->smoothed_psd[k]) +
                            ((1.0F - MARTIN_SMOOTH_ALPHA) * spectrum[k]);
  }

  // 2. Track minimum in current sub-window
  for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
    if (self->smoothed_psd[k] < self->current_subwin_min[k]) {
      self->current_subwin_min[k] = self->smoothed_psd[k];
    }
  }

  // 3. Check if sub-window is complete
  if (self->frame_count >= MARTIN_SUBWIN_LEN) {
    // Store sub-window minimum in history
    for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
      self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) +
                           self->subwin_index] = self->current_subwin_min[k];

      // Reset current sub-window min for next cycle
      self->current_subwin_min[k] = self->smoothed_psd[k];
    }

    self->subwin_index = (self->subwin_index + 1) % MARTIN_SUBWIN_COUNT;
    self->frame_count = 0;
  }

calculate_output:
  // 4. Calculate global minimum from history
  for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
    float min_val = self->current_subwin_min[k];
    for (uint32_t d = 0; d < MARTIN_SUBWIN_COUNT; d++) {
      float h_val = self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) + d];
      if (h_val < min_val) {
        min_val = h_val;
      }
    }

    // Apply bias correction to estimate the mean noise power from its minimum
    noise_spectrum[k] = min_val * MARTIN_BIAS_CORR;
  }

  self->frame_count++;
  return true;
}

void martin_noise_estimator_set_state(MartinNoiseEstimator* self,
                                      const float* initial_profile) {
  if (!self || !initial_profile) {
    return;
  }
  for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
    float val = fmaxf(initial_profile[k], FLT_MIN) / MARTIN_BIAS_CORR;
    self->smoothed_psd[k] = val;
    self->current_subwin_min[k] = val;
    for (uint32_t d = 0; d < MARTIN_SUBWIN_COUNT; d++) {
      self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) + d] = val;
    }
  }
  self->is_first_frame = false;
  self->frame_count = 0;
}

void martin_noise_estimator_update_seed(MartinNoiseEstimator* self,
                                        const float* seed_profile) {
  martin_noise_estimator_set_state(self, seed_profile);
}

void martin_noise_estimator_apply_floor(MartinNoiseEstimator* self,
                                        const float* floor_profile) {
  if (!self || !floor_profile) {
    return;
  }
  for (uint32_t k = 0; k < self->noise_spectrum_size; k++) {
    float floor_val = floor_profile[k];
    if (self->smoothed_psd[k] < floor_val) {
      self->smoothed_psd[k] = floor_val;
    }
    if (self->current_subwin_min[k] < floor_val) {
      self->current_subwin_min[k] = floor_val;
    }
    for (uint32_t d = 0; d < MARTIN_SUBWIN_COUNT; d++) {
      if (self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) + d] <
          floor_val) {
        self->subwin_history[((size_t)k * MARTIN_SUBWIN_COUNT) + d] = floor_val;
      }
    }
  }
}
