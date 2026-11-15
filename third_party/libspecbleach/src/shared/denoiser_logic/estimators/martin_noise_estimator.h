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

#ifndef MARTIN_NOISE_ESTIMATOR_H
#define MARTIN_NOISE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Minimum Statistics noise estimator based on:
 * Rainer Martin, "Noise Power Spectral Density Estimation Based on Optimal
 * Smoothing and Minimum Statistics," IEEE Transactions on Speech and Audio
 * Processing, vol. 9, no. 7, pp. 504-512, July 2001.
 */

typedef struct MartinNoiseEstimator MartinNoiseEstimator;

MartinNoiseEstimator* martin_noise_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size);

void martin_noise_estimator_free(MartinNoiseEstimator* self);

bool martin_noise_estimator_run(MartinNoiseEstimator* self,
                                const float* spectrum, float* noise_spectrum);

void martin_noise_estimator_set_state(MartinNoiseEstimator* self,
                                      const float* initial_profile);

void martin_noise_estimator_update_seed(MartinNoiseEstimator* self,
                                        const float* seed_profile);

void martin_noise_estimator_apply_floor(MartinNoiseEstimator* self,
                                        const float* floor_profile);

#endif
