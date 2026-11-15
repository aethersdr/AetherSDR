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

#include "shared/denoiser_logic/core/denoiser_profile_core.h"
#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/denoiser_logic/estimators/adaptive_noise_estimator.h"
#include "shared/denoiser_logic/estimators/noise_estimator.h"
#include "shared/utils/spectral_utils.h"
#include <math.h>

bool denoiser_profile_core_handle_learning_mode(NoiseEstimator* noise_estimator,
                                                float* reference_spectrum,
                                                int learn_noise_flag,
                                                bool* was_learning) {
  if (learn_noise_flag > 0) {
    // Learn all modes simultaneously
    for (int mode = ROLLING_MEAN; mode <= MINIMUM; mode++) {
      noise_estimation_run(noise_estimator, (NoiseEstimatorType)mode,
                           reference_spectrum);
    }
    *was_learning = true;
    return true;
  }

  if (*was_learning) {
    // User just stopped learning -> Finalize all captures
    for (int mode = ROLLING_MEAN; mode <= MINIMUM; mode++) {
      noise_estimation_finalize(noise_estimator, (NoiseEstimatorType)mode);
    }
    *was_learning = false;
  }

  return false;
}

void denoiser_profile_core_update(DenoiserProfileCoreParams params,
                                  const float* reference_spectrum) {
  if (params.adaptive_enabled && params.adaptive_estimator) {
    // Adaptive Denoising Mode
    int state_changed = *params.last_adaptive_state == 0;
    bool mode_changed =
        fabsf(*params.aggressiveness - params.param_aggressiveness) > 0.01f;

    if (state_changed || mode_changed) {
      // Calculate morphed base profile
      get_morphed_profile(params.manual_noise_floor,
                          get_noise_profile(params.noise_profile, ROLLING_MEAN),
                          get_noise_profile(params.noise_profile, MEDIAN),
                          get_noise_profile(params.noise_profile, MAX),
                          get_noise_profile(params.noise_profile, MINIMUM),
                          params.spectrum_size, params.param_aggressiveness);

      adaptive_estimator_update_seed(params.adaptive_estimator,
                                     params.manual_noise_floor);

      *params.last_adaptive_state = 1;
    }

    // Run adaptive estimator (handles smoothing and aggressiveness tracking)
    adaptive_estimator_run(params.adaptive_estimator, reference_spectrum,
                           params.noise_spectrum, params.aggressiveness,
                           params.param_aggressiveness);

    // Apply morphed profile as a floor
    adaptive_estimator_apply_floor(params.adaptive_estimator,
                                   params.manual_noise_floor);
    for (uint32_t k = 0U; k < params.spectrum_size; k++) {
      if (params.noise_spectrum[k] < params.manual_noise_floor[k]) {
        params.noise_spectrum[k] = params.manual_noise_floor[k];
      }
    }

  } else {
    // Manual Denoising Mode
    *params.last_adaptive_state = 0;

    // Use morphed profile
    get_morphed_profile(params.noise_spectrum,
                        get_noise_profile(params.noise_profile, ROLLING_MEAN),
                        get_noise_profile(params.noise_profile, MEDIAN),
                        get_noise_profile(params.noise_profile, MAX),
                        get_noise_profile(params.noise_profile, MINIMUM),
                        params.spectrum_size, params.param_aggressiveness);
  }
}
