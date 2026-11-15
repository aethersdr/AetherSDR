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

#ifndef SPECTRAL_SMOOTHER_H
#define SPECTRAL_SMOOTHER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum TimeSmoothingType {
  NO_SMOOTHING = 0,
  FIXED = 1,
  TRANSIENT_AWARE = 2,
} TimeSmoothingType;

typedef struct TimeSmoothingParameters {
  float smoothing;
} TimeSmoothingParameters;

typedef struct SpectralSmoother SpectralSmoother;

SpectralSmoother* spectral_smoothing_initialize(uint32_t fft_size,
                                                uint32_t sample_rate,
                                                TimeSmoothingType type);
void spectral_smoothing_free(SpectralSmoother* self);
bool spectral_smoothing_run(SpectralSmoother* self,
                            TimeSmoothingParameters parameters,
                            float* signal_spectrum);

void spectral_smoothing_apply_spatial(float* data, uint32_t size);
void spectral_smoothing_apply_simple_temporal(float* current, float* memory,
                                              uint32_t size, float smoothing);

#endif
