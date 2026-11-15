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

#ifndef NO_FLOOR_MANAGER_H
#define NO_FLOOR_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NoiseFloorManager NoiseFloorManager;

NoiseFloorManager* noise_floor_manager_initialize(uint32_t fft_size,
                                                  uint32_t sample_rate,
                                                  uint32_t hop);

void noise_floor_manager_free(NoiseFloorManager* self);

void noise_floor_manager_apply(NoiseFloorManager* self,
                               uint32_t real_spectrum_size, uint32_t fft_size,
                               float* gain_spectrum, const float* noise_profile,
                               float reduction_amount,
                               float tonal_reduction_amount,
                               const float* tonal_mask, float whitening_factor);

#endif
