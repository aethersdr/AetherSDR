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

#ifndef SHARED_DENOISER_LOGIC_NOISE_PROFILE_H
#define SHARED_DENOISER_LOGIC_NOISE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NoiseProfile NoiseProfile;

#define NOISE_PROFILE_MODES                                                    \
  4 // ROLLING_MEAN, MEDIAN, MAX, MINIMUM (no OFF storage needed)

NoiseProfile* noise_profile_initialize(uint32_t size);
void noise_profile_free(NoiseProfile* self);
float* get_noise_profile(NoiseProfile* self, int mode);
uint32_t get_noise_profile_size(NoiseProfile* self);
uint32_t get_noise_profile_block_count(NoiseProfile* self, int mode);
bool increment_block_count(NoiseProfile* self, int mode);
bool set_noise_profile(NoiseProfile* self, int mode, const float* noise_profile,
                       uint32_t noise_profile_size, uint32_t block_count);
void set_noise_profile_available(NoiseProfile* self, int mode);
bool reset_noise_profile(NoiseProfile* self);
bool is_noise_estimation_available(NoiseProfile* self, int mode);

#endif // SHARED_DENOISER_LOGIC_NOISE_PROFILE_H
