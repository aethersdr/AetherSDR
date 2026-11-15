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

#ifndef SPECTRAL_2D_DENOISER_H
#define SPECTRAL_2D_DENOISER_H

#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/spectral_processor.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Parameters for the 2D spectral denoiser.
 * Uses Non-Local Means algorithm for 2D smoothing of SNR map.
 */
typedef struct Denoiser2DParameters {
  int learn_noise;        /**< Learning mode: 0=disabled, 1=learn all modes */
  bool residual_listen;   /**< Output residue instead of denoised signal */
  float reduction_amount; /**< Gain floor / reduction amount (linear) */
  float smoothing_factor; /**< NLM 'h' parameter (smoothing strength) */
  float whitening_factor; /**< Whitening factor (0.0 to 1.0) */
  int adaptive_noise;     /**< Adaptive noise mode: 0=disabled, 1=enabled */
  int noise_estimation_method;  /**< 0=SPP-MMSE, 1=Brandt, 2=Martin MS */
  float nlm_masking_protection; /**< Masking protection depth (0.0 to 1.0) */
  float suppression_strength;   /**< Suppression aggressiveness (0.0 to 1.0) */
  float aggressiveness;  /**< -1.0 (Median/Min) to 1.0 (Max), 0.0 (Mean) */
  float tonal_reduction; /**< 0.0 to 1.0 (Phase 3) */
} Denoiser2DParameters;

/**
 * Initialize the 2D spectral denoiser.
 * @param sample_rate Audio sample rate in Hz
 * @param fft_size FFT size used by STFT
 * @param overlap_factor Overlap factor for STFT
 * @param noise_profile Shared noise profile (external ownership)
 * @return Handle to denoiser, or NULL on failure
 */
SpectralProcessorHandle spectral_2d_denoiser_initialize(
    uint32_t sample_rate, uint32_t fft_size, uint32_t overlap_factor,
    NoiseProfile* noise_profile);

/**
 * Free the 2D spectral denoiser.
 * @param instance Handle to denoiser
 */
void spectral_2d_denoiser_free(SpectralProcessorHandle instance);

/**
 * Load reduction parameters.
 * @param instance Handle to denoiser
 * @param parameters Denoising parameters
 * @return true on success
 */
bool load_2d_reduction_parameters(SpectralProcessorHandle instance,
                                  Denoiser2DParameters parameters);

/**
 * Run the 2D denoising on an FFT spectrum.
 * @param instance Handle to denoiser
 * @param fft_spectrum FFT spectrum buffer (will be modified in place)
 * @return true on success
 */
bool spectral_2d_denoiser_run(SpectralProcessorHandle instance,
                              float* fft_spectrum);

/**
 * Get additional latency introduced by NLM look-ahead.
 * @param instance Handle to denoiser
 * @return Latency in frames (multiply by hop for samples)
 */
uint32_t spectral_2d_denoiser_get_latency_frames(
    SpectralProcessorHandle instance);

#endif /* SPECTRAL_2D_DENOISER_H */
