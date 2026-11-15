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

#include "shared/denoiser_logic/estimators/spp_mmse_noise_estimator.h"
#include "shared/configurations.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct SppMmseNoiseEstimator {
  uint32_t noise_spectrum_size;
  float* spp_previous_noise_psd;
  float* spp_smoothed_spp;
  bool is_first_frame;
};

// SPP-MMSE helper functions
static float compute_spp_probability(float observation_power,
                                     float previous_noise_psd) {
  if (previous_noise_psd < SPECTRAL_EPSILON) {
    previous_noise_psd = SPECTRAL_EPSILON;
  }

  float ratio = observation_power / previous_noise_psd;
  float exponent = -ratio * (SPP_FIXED_XI_H1 / (1.F + SPP_FIXED_XI_H1));

  float exp_term = expf(exponent);
  if (!isfinite(exp_term)) {
    exp_term = (exponent > 0.F) ? FLT_MAX : 0.F;
  }

  float denominator_ratio = (1.F + SPP_FIXED_XI_H1) * exp_term;
  float spp = 1.F / (1.F + denominator_ratio);
  spp = fmaxf(0.F, fminf(1.F, spp));

  return spp;
}

static float compute_mmse_noise_estimate(float spp_h1, float spp_h0,
                                         float observation_power,
                                         float previous_noise_psd) {
  return (spp_h0 * observation_power) + (spp_h1 * previous_noise_psd);
}

SppMmseNoiseEstimator* spp_mmse_noise_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size) {
  (void)sample_rate;
  (void)fft_size;

  SppMmseNoiseEstimator* self =
      (SppMmseNoiseEstimator*)calloc(1U, sizeof(SppMmseNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->noise_spectrum_size = noise_spectrum_size;
  self->spp_previous_noise_psd =
      (float*)calloc(self->noise_spectrum_size, sizeof(float));
  self->spp_smoothed_spp =
      (float*)calloc(self->noise_spectrum_size, sizeof(float));

  if (!self->spp_previous_noise_psd || !self->spp_smoothed_spp) {
    spp_mmse_noise_estimator_free(self);
    return NULL;
  }

  self->is_first_frame = true;
  return self;
}

void spp_mmse_noise_estimator_free(SppMmseNoiseEstimator* self) {
  if (!self) {
    return;
  }
  free(self->spp_previous_noise_psd);
  free(self->spp_smoothed_spp);
  free(self);
}

bool spp_mmse_noise_estimator_run(SppMmseNoiseEstimator* self,
                                  const float* spectrum,
                                  float* noise_spectrum) {
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
    for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
      self->spp_previous_noise_psd[k] = spectrum[k];
      self->spp_smoothed_spp[k] = 0.F;
      noise_spectrum[k] = spectrum[k];
    }
    self->is_first_frame = false;
  } else {
    if (frame_energy < ESTIMATOR_SILENCE_THRESHOLD) {
      memcpy(noise_spectrum, self->spp_previous_noise_psd,
             sizeof(float) * self->noise_spectrum_size);
      return true;
    }

    for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
      float spp_h1 =
          compute_spp_probability(spectrum[k], self->spp_previous_noise_psd[k]);

      if (self->spp_smoothed_spp[k] > SPP_STAGNATION_CAP) {
        spp_h1 = fminf(spp_h1, SPP_STAGNATION_CAP);
      }
      float spp_h0 = 1.F - spp_h1;

      float mmse_noise_estimate = compute_mmse_noise_estimate(
          spp_h1, spp_h0, spectrum[k], self->spp_previous_noise_psd[k]);

      noise_spectrum[k] = (SPP_ALPHA_POW * self->spp_previous_noise_psd[k]) +
                          ((1.F - SPP_ALPHA_POW) * mmse_noise_estimate);

      self->spp_smoothed_spp[k] = (SPP_SMOOTH_SPP * self->spp_smoothed_spp[k]) +
                                  (SPP_CURRENT_SPP * spp_h1);

      self->spp_previous_noise_psd[k] = noise_spectrum[k];
    }
  }

  return true;
}

void spp_mmse_noise_estimator_set_state(SppMmseNoiseEstimator* self,
                                        const float* initial_profile) {
  if (!self || !initial_profile) {
    return;
  }

  for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
    float val = fmaxf(initial_profile[k], FLT_MIN);
    self->spp_previous_noise_psd[k] = val;
    self->spp_smoothed_spp[k] = 0.F;
  }

  self->is_first_frame = false;
}

void spp_mmse_noise_estimator_update_seed(SppMmseNoiseEstimator* self,
                                          const float* seed_profile) {
  if (!self || !seed_profile) {
    return;
  }

  for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
    float val = fmaxf(seed_profile[k], FLT_MIN);
    self->spp_previous_noise_psd[k] = val;
    self->spp_smoothed_spp[k] = 0.F;
  }
}

void spp_mmse_noise_estimator_apply_floor(SppMmseNoiseEstimator* self,
                                          const float* floor_profile) {
  if (!self || !floor_profile) {
    return;
  }

  for (uint32_t k = 0U; k < self->noise_spectrum_size; k++) {
    float floor_val = floor_profile[k];
    if (self->spp_previous_noise_psd[k] < floor_val) {
      self->spp_previous_noise_psd[k] = floor_val;
    }
  }
}
