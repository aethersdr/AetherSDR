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

#ifndef SPECBLEACH_DENOISER_H_INCLUDED
#define SPECBLEACH_DENOISER_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef void* SpectralBleachHandle;

typedef struct SpectralBleachDenoiserParameters {
  /* Sets the processor in listening mode to capture the noise profile.
   * 0 is disabled, 1 will learn all profile types simultaneously. */
  int learn_noise;

  /* Enables outputting the residue of the reduction processing. It's either
   * true or false */
  bool residual_listen;

  /* Sets the amount of dBs that the noise will be attenuated. It goes from 0 dB
   * to 40 dB. This controls both the gain attenuation strength and the residual
   * noise mixing level for optimal noise reduction. */
  float reduction_amount;

  /* Percentage of smoothing to apply. Averages the reduction calculation frame
   * per frame so the rate of change is less resulting in less musical noise but
   * if too strong it can blur transient and reduce high frequencies. It goes
   * from 0 to 100 percent */
  float smoothing_factor;

  /* Percentage of whitening that is going to be applied to the residue of the
   * reduction. It modifies the noise floor to be more like white noise. This
   * can help hide musical noise when the noise is colored. It goes from 0 to
   * 100 percent */
  float whitening_factor;

  /* Enables the adaptive noise estimation, which will continuously update the
   * noise profile based on the current input signal. */
  int adaptive_noise;

  /* Sets the method used for adaptive noise estimation.
   * 0: SPP-MMSE method (unbiased estimation)
   * 1: Brandt (Trimmed Mean)
   * 2: Martin Minimum Statistics */
  int noise_estimation_method;

  /** Masking Veto depth (0.0 - 1.0) */
  float masking_depth; // 0.0 - 1.0: Depth of signal energy preservation

  /** Suppression aggressiveness (0.0 - 1.0) */
  float suppression_strength; // 0.0 - 1.0: Berouti oversubtraction factor

  /* Intelligent Steering */
  float aggressiveness; /**< -1.0 (Median/Min) to 1.0 (Max), 0.0 (Mean) */
  /* Tonal Separation */
  float tonal_reduction; // 0.0 to 1.0: Independent reduction for tones
} SpectralBleachDenoiserParameters;

/**
 * Returns a handle to an instance of the library for the spectral based
 * noise reduction. Sample rate could be anything from 4000hz to 192khz.
 * Recommended frame size range is between 20ms and 100ms
 */
SpectralBleachHandle specbleach_initialize(uint32_t sample_rate,
                                           float frame_size);
/**
 * Free instance associated to the handle passed
 */
void specbleach_free(SpectralBleachHandle instance);
/**
 * Loads the parameters for the reduction.
 * This has to be called before processing
 */
bool specbleach_load_parameters(SpectralBleachHandle instance,
                                SpectralBleachDenoiserParameters parameters);
/**
 * Process buffer of a number of samples
 */
bool specbleach_process(SpectralBleachHandle instance,
                        uint32_t number_of_samples, const float* input,
                        float* output);
/**
 * Returns the latency in samples associated with the library instance
 */
uint32_t specbleach_get_latency(SpectralBleachHandle instance);
/**
 * Returns the size of the noise profile spectrum
 */
uint32_t specbleach_get_noise_profile_size(SpectralBleachHandle instance);
/**
 * Allows to load a custom noise profile for a specific mode
 */
bool specbleach_load_noise_profile_for_mode(SpectralBleachHandle instance,
                                            const float* restored_profile,
                                            uint32_t profile_size,
                                            uint32_t block_count, int mode);
/**
 * Resets the internal noise profile of the library instance
 */
bool specbleach_reset_noise_profile(SpectralBleachHandle instance);

/**
 * Returns the number of blocks used for the noise profile calculation for a
 * specific mode
 */
uint32_t specbleach_get_noise_profile_block_count_for_mode(
    SpectralBleachHandle instance, int mode);

/**
 * Returns a pointer to the noise profile for a specific mode
 */
float* specbleach_get_noise_profile_for_mode(SpectralBleachHandle instance,
                                             int mode);

/**
 * Returns if the instance has a noise profile calculated for a specific mode
 */
bool specbleach_noise_profile_available_for_mode(SpectralBleachHandle instance,
                                                 int mode);

#ifdef __cplusplus
}
#endif
#endif
