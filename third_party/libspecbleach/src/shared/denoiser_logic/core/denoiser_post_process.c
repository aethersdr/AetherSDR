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

#include "shared/denoiser_logic/core/denoiser_post_process.h"
#include "shared/denoiser_logic/core/noise_floor_manager.h"
#include "shared/denoiser_logic/processing/tonal_reducer.h"
#include "shared/utils/spectral_utils.h"
#include <stdlib.h>

void denoiser_post_process_apply(DenoiserPostProcessParams params) {
  // 1. Apply noise floor management
  noise_floor_manager_apply(
      params.noise_floor_manager, params.real_spectrum_size, params.fft_size,
      params.gain_spectrum, params.noise_spectrum, params.reduction_amount,
      params.tonal_reduction, tonal_reducer_get_mask(params.tonal_reducer),
      params.whitening_factor);

  // 2. Mixing Logic (formerly denoise_mixer)
  // We perform mixing in-place or with stack variables avoiding extra heap
  // allocation if possible However, to calculate residual we need the denoised
  // version and original. params.fft_spectrum holds the original.

  // We can optimize this by calculating per-bin.

  if (params.residual_listen) {
    // Residual Listen Mode: Output = Original - Denoised
    // Original - (Original * Gain) = Original * (1 - Gain)
    for (uint32_t k = 0U; k < params.fft_size; k++) {
      float original = params.fft_spectrum[k];
      float gain = params.gain_spectrum[k];
      float denoised = original * gain;
      params.fft_spectrum[k] = original - denoised;
    }
  } else {
    // Normal Mode: Output = Original * Gain
    for (uint32_t k = 0U; k < params.fft_size; k++) {
      params.fft_spectrum[k] *= params.gain_spectrum[k];
    }
  }
}
