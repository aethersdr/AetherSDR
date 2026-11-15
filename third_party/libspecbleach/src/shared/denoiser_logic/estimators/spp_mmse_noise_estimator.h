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

#ifndef SPP_MMSE_NOISE_ESTIMATOR_H
#define SPP_MMSE_NOISE_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SppMmseNoiseEstimator SppMmseNoiseEstimator;

SppMmseNoiseEstimator* spp_mmse_noise_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size);

void spp_mmse_noise_estimator_free(SppMmseNoiseEstimator* self);

bool spp_mmse_noise_estimator_run(SppMmseNoiseEstimator* self,
                                  const float* spectrum, float* noise_spectrum);

void spp_mmse_noise_estimator_set_state(SppMmseNoiseEstimator* self,
                                        const float* initial_profile);

void spp_mmse_noise_estimator_update_seed(SppMmseNoiseEstimator* self,
                                          const float* seed_profile);

void spp_mmse_noise_estimator_apply_floor(SppMmseNoiseEstimator* self,
                                          const float* floor_profile);

#endif
