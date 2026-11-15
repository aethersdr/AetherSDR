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

/* removed old path include */
#include "shared/denoiser_logic/estimators/adaptive_noise_estimator.h"
#include "shared/configurations.h"
#include "shared/denoiser_logic/estimators/brandt_noise_estimator.h"
#include "shared/denoiser_logic/estimators/martin_noise_estimator.h"
#include "shared/denoiser_logic/estimators/spp_mmse_noise_estimator.h"
#include <stdlib.h>

struct AdaptiveNoiseEstimator {
  AdaptiveNoiseEstimationMethod method;
  uint32_t spectrum_size;
  void* internal_estimator;
};

static AdaptiveNoiseEstimator* create_spp_mmse_estimator(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size) {
  AdaptiveNoiseEstimator* self =
      (AdaptiveNoiseEstimator*)calloc(1U, sizeof(AdaptiveNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->method = SPP_MMSE_METHOD;
  self->spectrum_size = noise_spectrum_size;
  self->internal_estimator = spp_mmse_noise_estimator_initialize(
      noise_spectrum_size, sample_rate, fft_size);

  if (!self->internal_estimator) {
    free(self);
    return NULL;
  }

  return self;
}

static AdaptiveNoiseEstimator* create_brandt_estimator(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size) {
  AdaptiveNoiseEstimator* self =
      (AdaptiveNoiseEstimator*)calloc(1U, sizeof(AdaptiveNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->method = BRANDT_METHOD;
  self->spectrum_size = noise_spectrum_size;
  // Default parameters: 5000ms history (extremely robust for music)
  self->internal_estimator = brandt_noise_estimator_initialize(
      noise_spectrum_size, BRANDT_DEFAULT_HISTORY_MS, sample_rate, fft_size);

  if (!self->internal_estimator) {
    free(self);
    return NULL;
  }

  return self;
}

static AdaptiveNoiseEstimator* create_martin_estimator(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size) {
  AdaptiveNoiseEstimator* self =
      (AdaptiveNoiseEstimator*)calloc(1U, sizeof(AdaptiveNoiseEstimator));
  if (!self) {
    return NULL;
  }

  self->method = MARTIN_METHOD;
  self->spectrum_size = noise_spectrum_size;
  self->internal_estimator = martin_noise_estimator_initialize(
      noise_spectrum_size, sample_rate, fft_size);

  if (!self->internal_estimator) {
    free(self);
    return NULL;
  }

  return self;
}

static bool run_martin(AdaptiveNoiseEstimator* self, const float* spectrum,
                       float* noise_spectrum) {
  return martin_noise_estimator_run(
      (MartinNoiseEstimator*)self->internal_estimator, spectrum,
      noise_spectrum);
}

static bool run_brandt(AdaptiveNoiseEstimator* self, const float* spectrum,
                       float* noise_spectrum) {
  return brandt_noise_estimator_run(
      (BrandtNoiseEstimator*)self->internal_estimator, spectrum,
      noise_spectrum);
}

AdaptiveNoiseEstimator* adaptive_estimator_initialize(
    uint32_t noise_spectrum_size, uint32_t sample_rate, uint32_t fft_size,
    AdaptiveNoiseEstimationMethod method) {
  if (method == SPP_MMSE_METHOD) {
    return create_spp_mmse_estimator(noise_spectrum_size, sample_rate,
                                     fft_size);
  }
  if (method == BRANDT_METHOD) {
    return create_brandt_estimator(noise_spectrum_size, sample_rate, fft_size);
  }
  if (method == MARTIN_METHOD) {
    return create_martin_estimator(noise_spectrum_size, sample_rate, fft_size);
  }
  return create_martin_estimator(noise_spectrum_size, sample_rate, fft_size);
}

AdaptiveNoiseEstimationMethod adaptive_estimator_get_method(
    const AdaptiveNoiseEstimator* self) {
  if (!self) {
    return MARTIN_METHOD; // Safe default
  }
  return self->method;
}

void adaptive_estimator_free(AdaptiveNoiseEstimator* self) {
  if (!self) {
    return;
  }

  if (self->method == SPP_MMSE_METHOD) {
    spp_mmse_noise_estimator_free(
        (SppMmseNoiseEstimator*)self->internal_estimator);
  } else if (self->method == BRANDT_METHOD) {
    brandt_noise_estimator_free(
        (BrandtNoiseEstimator*)self->internal_estimator);
  } else if (self->method == MARTIN_METHOD) {
    martin_noise_estimator_free(
        (MartinNoiseEstimator*)self->internal_estimator);
  }

  free(self);
}

bool adaptive_estimator_run(AdaptiveNoiseEstimator* self, const float* spectrum,
                            float* noise_spectrum, float* aggressiveness,
                            float param_aggressiveness) {
  if (!self) {
    return false;
  }

  bool success = false;
  if (self->method == SPP_MMSE_METHOD) {
    success = spp_mmse_noise_estimator_run(
        (SppMmseNoiseEstimator*)self->internal_estimator, spectrum,
        noise_spectrum);
  } else if (self->method == MARTIN_METHOD) {
    success = run_martin(self, spectrum, noise_spectrum);
  } else {
    success = run_brandt(self, spectrum, noise_spectrum);
  }

  if (success) {
    // Update internal aggressiveness state
    if (aggressiveness) {
      *aggressiveness = param_aggressiveness;
    }

    // Refine the estimator output: heal spectral gaps before smoothing
    interpolate_spectrum_gaps(noise_spectrum, self->spectrum_size,
                              NOISE_ESTIMATION_INTERPOLATION_THRESHOLD);

    // Smooth the estimator output to eliminate residual musical noise
    smooth_spectrum(noise_spectrum, self->spectrum_size,
                    ADAPTIVE_NOISE_FLOOR_SMOOTHING);
  }

  return success;
}

void adaptive_estimator_set_state(AdaptiveNoiseEstimator* self,
                                  const float* initial_profile,
                                  int estimation_method) {
  (void)estimation_method;
  if (!self) {
    return;
  }

  if (self->method == SPP_MMSE_METHOD) {
    spp_mmse_noise_estimator_set_state(
        (SppMmseNoiseEstimator*)self->internal_estimator, initial_profile);
  } else if (self->method == MARTIN_METHOD) {
    martin_noise_estimator_set_state(
        (MartinNoiseEstimator*)self->internal_estimator, initial_profile);
  } else {
    brandt_noise_estimator_set_state(
        (BrandtNoiseEstimator*)self->internal_estimator, initial_profile);
  }
}

void adaptive_estimator_apply_floor(AdaptiveNoiseEstimator* self,
                                    const float* floor_profile) {
  if (!self) {
    return;
  }

  if (self->method == SPP_MMSE_METHOD) {
    spp_mmse_noise_estimator_apply_floor(
        (SppMmseNoiseEstimator*)self->internal_estimator, floor_profile);
  } else if (self->method == MARTIN_METHOD) {
    martin_noise_estimator_apply_floor(
        (MartinNoiseEstimator*)self->internal_estimator, floor_profile);
  } else {
    brandt_noise_estimator_apply_floor(
        (BrandtNoiseEstimator*)self->internal_estimator, floor_profile);
  }
}

void adaptive_estimator_update_seed(AdaptiveNoiseEstimator* self,
                                    const float* seed_profile) {
  if (!self) {
    return;
  }

  if (self->method == SPP_MMSE_METHOD) {
    spp_mmse_noise_estimator_update_seed(
        (SppMmseNoiseEstimator*)self->internal_estimator, seed_profile);
  } else if (self->method == MARTIN_METHOD) {
    martin_noise_estimator_update_seed(
        (MartinNoiseEstimator*)self->internal_estimator, seed_profile);
  } else {
    brandt_noise_estimator_update_seed(
        (BrandtNoiseEstimator*)self->internal_estimator, seed_profile);
  }
}
