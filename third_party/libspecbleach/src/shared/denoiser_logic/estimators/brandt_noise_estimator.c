/*
libspecbleach - A spectral processing library

Copyright 2026 Luciano Dato <lucianodato@gmail.com>

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

#include "shared/denoiser_logic/estimators/brandt_noise_estimator.h"
#include "shared/configurations.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct BrandtNoiseEstimator {
  uint32_t spectrum_size;
  uint32_t history_size;
  uint32_t history_index; // Circular buffer write head

  float* history_buffer;      // Size: spectrum_size * history_size
  float* sort_buffer;         // Scratch space for sorting (Size: history_size)
  float* last_noise_spectrum; // Persisted noise to keep during rejection

  float percentile;
  uint32_t trim_count; // Number of items to average (history_size * percentile)
  float correction_factor;
  float correction_factors[5]; // Pre-calculated for search
  bool is_first_frame;
};

// Helper: Calculate bias correction factor for trimmed mean of exponential dist
// Formula: Factor = 1 / (1 + ( (1-P)/P * ln(1-P) ))
static float calculate_correction_factor(float p) {
  if (p <= 0.0f || p >= 1.0f) {
    return 1.0f; // Invalid P, no correction
  }
  float term = (1.0f - p) / p * logf(1.0f - p);
  float denominator = 1.0f + term;
  if (fabsf(denominator) < BRANDT_ESTIMATOR_BIAS_EPSILON) {
    return 1.0f; // Avoid division by zero
  }
  return 1.0f / denominator;
}

// Helper: Compare floats for qsort
static int compare_floats(const void* a, const void* b) {
  float fa = *(const float*)a;
  float fb = *(const float*)b;
  return (fa > fb) - (fa < fb);
}

BrandtNoiseEstimator* brandt_noise_estimator_initialize(
    uint32_t spectrum_size, float history_duration_ms, uint32_t sample_rate,
    uint32_t fft_size) {
  float percentile = BRANDT_DEFAULT_PERCENTILE; // Advised for music restoration
  BrandtNoiseEstimator* self =
      (BrandtNoiseEstimator*)calloc(1, sizeof(BrandtNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->spectrum_size = spectrum_size;
  self->percentile = percentile;

  // Calculate history size from duration
  float ms_per_frame = (float)fft_size * 1000.0f / (float)sample_rate;
  // Overlap consideration: Usually frame step is hop_size.
  // Assuming hop = fft_size/2 or similar? The caller usually provides
  // parameters. If exact duration needed, we might need hop_size.
  // Assuming standard 50% overlap for calculation roughly:
  float frame_duration = ms_per_frame * 0.5f; // Rough approximation of step
  if (frame_duration < BRANDT_ESTIMATOR_MIN_DURATION_MS) {
    frame_duration = BRANDT_ESTIMATOR_MIN_DURATION_MS;
  }

  self->history_size = (uint32_t)(history_duration_ms / frame_duration);
  if (self->history_size < BRANDT_ESTIMATOR_MIN_HISTORY_FRAMES) {
    self->history_size = BRANDT_ESTIMATOR_MIN_HISTORY_FRAMES; // Minimum history
  }

  self->trim_count = (uint32_t)((float)self->history_size * percentile);
  if (self->trim_count < 1) {
    self->trim_count = 1; // At least min
  }
  if (self->trim_count > self->history_size) {
    self->trim_count = self->history_size;
  }

  self->correction_factor = calculate_correction_factor(percentile);

  static const float p_candidates[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f};
  for (int i = 0; i < 5; i++) {
    self->correction_factors[i] = calculate_correction_factor(p_candidates[i]);
  }

  // Allocate buffers
  self->history_buffer = (float*)calloc(
      (size_t)self->spectrum_size * self->history_size, sizeof(float));
  self->sort_buffer = (float*)calloc(self->history_size, sizeof(float));
  self->last_noise_spectrum =
      (float*)calloc(self->spectrum_size, sizeof(float));

  if (!self->history_buffer || !self->sort_buffer ||
      !self->last_noise_spectrum) {
    brandt_noise_estimator_free(self);
    return NULL;
  }

  self->is_first_frame = true;
  return self;
}

void brandt_noise_estimator_free(BrandtNoiseEstimator* self) {
  if (self) {
    free(self->history_buffer);
    free(self->sort_buffer);
    free(self->last_noise_spectrum);
    free(self);
  }
}

static float calculate_ad_norm(const float* sorted, uint32_t q, float mu,
                               float b) {
  if (mu < 1e-15f) {
    return 1.0f;
  }
  float mu_inv = 1.0f / mu;
  float exp_b_mu = expf(-b * mu_inv);
  float denom = 1.0f - exp_b_mu;
  if (fabsf(denom) < 1e-12f) {
    return 1.0f;
  }

  float abs_diff_sum = 0.0f;
  float q_inv = 1.0f / (float)q;
  float denom_inv = 1.0f / denom;

  // Most expensive loop in denoiser: 5 * spectrum_size * q calls per frame
  // q is roughly history_size / 2.
  for (uint32_t i = 0; i < q; i++) {
    float f_te = (1.0f - expf(-sorted[i] * mu_inv)) * denom_inv;
    float f_empirical = (float)(i + 1) * q_inv;
    abs_diff_sum += fabsf(f_empirical - f_te);
  }
  return abs_diff_sum * (2.0f * q_inv);
}

bool brandt_noise_estimator_run(BrandtNoiseEstimator* self,
                                const float* spectrum, float* noise_spectrum) {
  if (!self || !spectrum || !noise_spectrum || self->history_size == 0) {
    return false;
  }

  float frame_energy = 0.F;
  for (uint32_t k = 0U; k < self->spectrum_size; k++) {
    frame_energy += spectrum[k];
  }
  frame_energy /= (float)self->spectrum_size;

  if (self->is_first_frame) {
    if (frame_energy > ESTIMATOR_SILENCE_THRESHOLD) {
      float inv_factor = 1.0f / calculate_correction_factor(0.5f);
      for (uint32_t k = 0; k < self->spectrum_size; k++) {
        float val = spectrum[k] * inv_factor;
        self->last_noise_spectrum[k] = spectrum[k];
        for (uint32_t t = 0; t < self->history_size; t++) {
          float jitter =
              1.0f + (0.01f * (float)(((int32_t)(t + k) % 11) - 5) / 5.0f);
          self->history_buffer[((size_t)k * self->history_size) + t] =
              val * jitter;
        }
      }
      self->is_first_frame = false;
    }
  }

  if (frame_energy < ESTIMATOR_SILENCE_THRESHOLD) {
    memcpy(noise_spectrum, self->last_noise_spectrum,
           self->spectrum_size * sizeof(float));
    return true;
  }

  for (uint32_t k = 0; k < self->spectrum_size; k++) {
    self->history_buffer[((size_t)k * self->history_size) +
                         self->history_index] = spectrum[k];
  }
  self->history_index = (self->history_index + 1) % self->history_size;

  static const float p_candidates[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.0f};

  for (uint32_t k = 0; k < self->spectrum_size; k++) {
    float* bin_history = &self->history_buffer[(size_t)k * self->history_size];
    memcpy(self->sort_buffer, bin_history, self->history_size * sizeof(float));
    qsort(self->sort_buffer, self->history_size, sizeof(float), compare_floats);

    float min_ad_norm = 2.0f;
    float best_mu = self->last_noise_spectrum[k];

    for (int i = 0; i < 5; i++) {
      float p = p_candidates[i];
      uint32_t q = (uint32_t)(p * (float)self->history_size);
      if (q < 10) {
        continue;
      }

      float b = self->sort_buffer[q - 1];
      float sum = 0.0f;
      for (uint32_t j = 0; j < q; j++) {
        sum += self->sort_buffer[j];
      }
      float mu_trunc = sum / (float)q;

      if (mu_trunc > ESTIMATOR_SILENCE_THRESHOLD) {
        float factor = self->correction_factors[i];
        float mu_full = mu_trunc * factor;
        float ad_norm = calculate_ad_norm(self->sort_buffer, q, mu_full, b);
        if (ad_norm < min_ad_norm) {
          min_ad_norm = ad_norm;
          best_mu = mu_full;
        }
      }
    }

    if (1.0f - min_ad_norm >= BRANDT_MIN_CONFIDENCE) {
      self->last_noise_spectrum[k] = best_mu;
    }
    noise_spectrum[k] = self->last_noise_spectrum[k];
  }

  return true;
}

void brandt_noise_estimator_set_state(BrandtNoiseEstimator* self,
                                      const float* initial_profile) {
  if (!self || !initial_profile) {
    return;
  }

  // Start with a known state: Fill history with this profile
  float inverse_factor = 1.0f / self->correction_factor;

  for (uint32_t k = 0; k < self->spectrum_size; k++) {
    float val = initial_profile[k] * inverse_factor;
    self->last_noise_spectrum[k] = initial_profile[k];
    for (uint32_t t = 0; t < self->history_size; t++) {
      float jitter =
          1.0f + (0.01f * (float)(((int32_t)(t + k) % 11) - 5) / 5.0f);
      self->history_buffer[((size_t)k * self->history_size) + t] = val * jitter;
    }
  }
  self->is_first_frame = false;
}

void brandt_noise_estimator_update_seed(BrandtNoiseEstimator* self,
                                        const float* seed_profile) {
  if (!self || !seed_profile) {
    return;
  }
  // Similar to set_state but maybe only updates part of history?
  // For now, treat same as set_state to ensure quick convergence/reset.
  brandt_noise_estimator_set_state(self, seed_profile);
  self->is_first_frame = false;
}

void brandt_noise_estimator_apply_floor(BrandtNoiseEstimator* self,
                                        const float* floor_profile) {
  if (!self || !floor_profile) {
    return;
  }
  float inverse_factor = 1.0f / self->correction_factor;
  for (uint32_t k = 0; k < self->spectrum_size; k++) {
    float floor_val = floor_profile[k] * inverse_factor;
    for (uint32_t t = 0; t < self->history_size; t++) {
      if (self->history_buffer[((size_t)k * self->history_size) + t] <
          floor_val) {
        self->history_buffer[((size_t)k * self->history_size) + t] = floor_val;
      }
    }
  }
}

void brandt_noise_estimator_set_history_duration(
    const BrandtNoiseEstimator* self, float history_duration_ms,
    uint32_t sample_rate, uint32_t fft_size) {
  if (!self) {
    return;
  }

  // To avoid frequent reallocations, we only update if it's a significant
  // change For now, let's just update the internal logic if we don't want to
  // realloc. Actually, changing history size at runtime is best done by
  // pre-allocating a MAX size. Given we are in the middle of a fix, I will only
  // expose Percentile as "Sensitivity" first.
  (void)history_duration_ms;
  (void)sample_rate;
  (void)fft_size;
}
