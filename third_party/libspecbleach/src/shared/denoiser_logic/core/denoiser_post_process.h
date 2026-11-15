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

#ifndef SHARED_DENOISER_POST_PROCESS_H
#define SHARED_DENOISER_POST_PROCESS_H

#include "shared/denoiser_logic/core/noise_floor_manager.h"
#include "shared/denoiser_logic/processing/tonal_reducer.h"
#include <stdbool.h>
#include <stdint.h>

// Core parameters for post-processing and mixing
typedef struct DenoiserPostProcessParams {
  uint32_t fft_size;
  uint32_t real_spectrum_size;
  float reduction_amount;
  float tonal_reduction;
  float whitening_factor;
  bool residual_listen;

  // Components
  NoiseFloorManager* noise_floor_manager;
  TonalReducer* tonal_reducer;

  // Buffers
  float* gain_spectrum;
  const float* noise_spectrum;
  float* fft_spectrum; // Input/Output spectrum (modified in place)
} DenoiserPostProcessParams;

/**
 * @brief Applies post-processing steps: Noise Floor Management and Mixing.
 *
 * This function handles:
 * 1. Application of the noise floor manager to the gain spectrum.
 * 2. Application of the gains to the input spectrum (mixing).
 * 3. Calculation of the residual if needed.
 * 4. Output selection based on residual_listen flag.
 *
 * @param params Struct containing all necessary parameters and component
 * pointers.
 */
void denoiser_post_process_apply(DenoiserPostProcessParams params);

#endif
