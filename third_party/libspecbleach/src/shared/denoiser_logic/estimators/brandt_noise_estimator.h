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

#ifndef BRANDT_NOISE_ESTIMATOR_H
#define BRANDT_NOISE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BrandtNoiseEstimator BrandtNoiseEstimator;

/**
 * @brief Creates a new Brandt (Trimmed Mean) noise estimator instance
 *
 * Implements the algorithm from:
 * "Automatic Noise PSD Estimation for Restoration of Archived Audio"
 * Brandt et al. (2017).
 *
 * Uses a trimmed mean of the past N frames (lowest P%) and applies
 * a bias correction factor assuming exponential distribution.
 *
 * @param spectrum_size Number of frequency bins
 * @param history_duration_ms Duration of the sliding window in milliseconds
 * @param percentile The percentile to trim at (0.0 - 1.0). e.g., 0.15 for 15%.
 * @param sample_rate Audio sample rate
 * @param fft_size FFT size (for time/freq conversion)
 * @return BrandtNoiseEstimator* or NULL on failure
 */
BrandtNoiseEstimator* brandt_noise_estimator_initialize(
    uint32_t spectrum_size, float history_duration_ms, uint32_t sample_rate,
    uint32_t fft_size);

/**
 * @brief Destroys the estimator instance
 */
void brandt_noise_estimator_free(BrandtNoiseEstimator* self);

/**
 * @brief Process a new spectral frame and update the noise estimate
 *
 * @param self Estimator instance
 * @param spectrum Current Input power/magnitude spectrum
 * @param noise_spectrum Output: Updated noise spectrum
 * @return true if successful
 */
bool brandt_noise_estimator_run(BrandtNoiseEstimator* self,
                                const float* spectrum, float* noise_spectrum);

/**
 * @brief Force the internal stated to a specific profile
 */
void brandt_noise_estimator_set_state(BrandtNoiseEstimator* self,
                                      const float* initial_profile);

/**
 * @brief Update the history seed without full reset
 */
void brandt_noise_estimator_update_seed(BrandtNoiseEstimator* self,
                                        const float* seed_profile);

/**
 * @brief Apply a minimum floor to the estimate
 */
void brandt_noise_estimator_apply_floor(BrandtNoiseEstimator* self,
                                        const float* floor_profile);

void brandt_noise_estimator_set_history_duration(
    const BrandtNoiseEstimator* self, float history_duration_ms,
    uint32_t sample_rate, uint32_t fft_size);

#endif
