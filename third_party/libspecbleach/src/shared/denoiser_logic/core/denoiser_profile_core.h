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

#ifndef SHARED_DENOISER_PROFILE_CORE_H
#define SHARED_DENOISER_PROFILE_CORE_H

#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/denoiser_logic/estimators/adaptive_noise_estimator.h"
#include "shared/denoiser_logic/estimators/noise_estimator.h"
#include <stdbool.h>
#include <stdint.h>

// Core parameters needed for updating noise profile
typedef struct DenoiserProfileCoreParams {
  bool adaptive_enabled;
  uint32_t spectrum_size;
  float* aggressiveness;
  float param_aggressiveness;
  int* last_adaptive_state; // Pointer to state variable in parent struct

  // Components
  AdaptiveNoiseEstimator* adaptive_estimator;
  NoiseProfile* noise_profile;
  float* manual_noise_floor;
  float* noise_spectrum; // Output buffer
} DenoiserProfileCoreParams;

/**
 * @brief Handles the learning mode logic.
 *
 * @param noise_estimator The noise estimator instance.
 * @param reference_spectrum The current frame's power spectrum.
 * @param learn_noise_flag Whether learning mode is actively requested (>0).
 * @param was_learning Pointer to the state variable tracking if learning was
 * active.
 * @return true if learning was processed (caller should return early), false
 * otherwise.
 */
bool denoiser_profile_core_handle_learning_mode(NoiseEstimator* noise_estimator,
                                                float* reference_spectrum,
                                                int learn_noise_flag,
                                                bool* was_learning);

/**
 * @brief Updates the noise profile based on adaptive or manual mode settings.
 *
 * This function encapsulates the logic for switching between manual and
 * adaptive modes, handling state transitions, morphing the base profile, and
 * running the adaptive estimator.
 *
 * @param params Struct containing all necessary improved parameters and
 * component pointers.
 */
void denoiser_profile_core_update(DenoiserProfileCoreParams params,
                                  const float* reference_spectrum);

#endif
