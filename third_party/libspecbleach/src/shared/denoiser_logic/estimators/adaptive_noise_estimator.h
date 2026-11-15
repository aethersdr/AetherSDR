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

#ifndef ADAPTIVE_NOISE_ESTIMATOR_H
#define ADAPTIVE_NOISE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum AdaptiveNoiseEstimationMethod {
  SPP_MMSE_METHOD = 0, // Speech Presence Probability - MMSE method
  BRANDT_METHOD = 1,   // Trimmed Mean (Automatic PSD Estimation - 2017)
  MARTIN_METHOD = 2,   // Martin (2001) Minimum Statistics
} AdaptiveNoiseEstimationMethod;

typedef struct AdaptiveNoiseEstimator AdaptiveNoiseEstimator;

// Create an adaptive noise estimator with the specified method
AdaptiveNoiseEstimator* adaptive_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size,
    AdaptiveNoiseEstimationMethod method);

// Get the estimation method of an existing estimator
AdaptiveNoiseEstimationMethod adaptive_estimator_get_method(
    const AdaptiveNoiseEstimator* self);

// Free an adaptive noise estimator
void adaptive_estimator_free(AdaptiveNoiseEstimator* self);

// Run the estimator (dispatches to appropriate internal method)
bool adaptive_estimator_run(AdaptiveNoiseEstimator* self, const float* spectrum,
                            float* noise_spectrum, float* aggressiveness,
                            float param_aggressiveness);

// Set the internal state of the estimator from an existing noise profile
void adaptive_estimator_set_state(AdaptiveNoiseEstimator* self,
                                  const float* initial_profile,
                                  int estimation_method);

// Apply a minimum floor to the internal state of the estimator
void adaptive_estimator_apply_floor(AdaptiveNoiseEstimator* self,
                                    const float* floor_profile);

// Update the starting point (seed) of the estimator without resetting
void adaptive_estimator_update_seed(AdaptiveNoiseEstimator* self,
                                    const float* seed_profile);

#endif
