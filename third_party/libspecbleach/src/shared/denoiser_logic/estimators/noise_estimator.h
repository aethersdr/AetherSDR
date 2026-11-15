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

#ifndef SHARED_DENOISER_LOGIC_NOISE_ESTIMATOR_H
#define SHARED_DENOISER_LOGIC_NOISE_ESTIMATOR_H

#include "shared/denoiser_logic/core/noise_profile.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct NoiseEstimator NoiseEstimator;

typedef enum NoiseEstimatorType {
  OFF = 0,
  ROLLING_MEAN = 1,
  MEDIAN = 2,
  MAX = 3,
  MINIMUM = 4
} NoiseEstimatorType;

NoiseEstimator* noise_estimation_initialize(uint32_t fft_size,
                                            NoiseProfile* noise_profile);
void noise_estimation_free(NoiseEstimator* self);
bool noise_estimation_run(NoiseEstimator* self,
                          NoiseEstimatorType noise_estimator_type,
                          float* signal_spectrum);
void noise_estimation_finalize(NoiseEstimator* self,
                               NoiseEstimatorType noise_estimator_type);

#endif // SHARED_DENOISER_LOGIC_NOISE_ESTIMATOR_H
