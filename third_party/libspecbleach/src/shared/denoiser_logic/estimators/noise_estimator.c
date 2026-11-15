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

#include "shared/denoiser_logic/estimators/noise_estimator.h"
#include "shared/configurations.h"
#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/utils/spectral_circular_buffer.h"
#include "shared/utils/spectral_utils.h"
#include <stdlib.h>
#include <string.h>

struct NoiseEstimator {
  uint32_t fft_size;
  uint32_t real_spectrum_size;
  SbSpectralCircularBuffer* median_buffer;
  uint32_t layer_median;

  NoiseProfile* noise_profile;
};

NoiseEstimator* noise_estimation_initialize(const uint32_t fft_size,
                                            NoiseProfile* noise_profile) {
  NoiseEstimator* self = (NoiseEstimator*)calloc(1U, sizeof(NoiseEstimator));

  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;

  self->noise_profile = noise_profile;
  self->median_buffer =
      spectral_circular_buffer_create(NUMBER_OF_MEDIAN_SPECTRUM);

  if (!self->median_buffer) {
    noise_estimation_free(self);
    return NULL;
  }

  self->layer_median = spectral_circular_buffer_add_layer(
      self->median_buffer, self->real_spectrum_size);

  if (!self->median_buffer) {
    noise_estimation_free(self);
    return NULL;
  }

  return self;
}

void noise_estimation_free(NoiseEstimator* self) {
  if (!self) {
    return;
  }

  // Don't free noise profile used as reference here

  if (self->median_buffer) {
    spectral_circular_buffer_free(self->median_buffer);
  }

  free(self);
}

bool noise_estimation_run(NoiseEstimator* self,
                          const NoiseEstimatorType noise_estimator_type,
                          float* signal_spectrum) {
  if (!self || !signal_spectrum) {
    return false;
  }

  float* noise_profile =
      get_noise_profile(self->noise_profile, noise_estimator_type);

  switch (noise_estimator_type) {
    case ROLLING_MEAN:
      get_rolling_mean_spectrum(noise_profile, signal_spectrum,
                                get_noise_profile_block_count(
                                    self->noise_profile, noise_estimator_type),
                                self->real_spectrum_size);
      increment_block_count(self->noise_profile, noise_estimator_type);
      break;
    case MEDIAN: {
      spectral_circular_buffer_push(self->median_buffer, self->layer_median,
                                    signal_spectrum);
      spectral_circular_buffer_advance(self->median_buffer);

      const uint32_t blocks = NUMBER_OF_MEDIAN_SPECTRUM;
      const float* history_frames[blocks];

      // Retrieve history (0 = most recent, blocks-1 = oldest)
      for (uint32_t i = 0; i < blocks; i++) {
        // We retrieve with delay 'i + 1' because 'advance' was already called
        // so current write index is at +1 relative to the frame we just pushed.
        // wait, usually we push then advance.
        // Retrieve(delay=1) gets the frame we just pushed.
        history_frames[i] = spectral_circular_buffer_retrieve(
            self->median_buffer, self->layer_median, i + 1);
      }

      bool is_valid_median = get_rolling_median_spectrum(
          noise_profile, history_frames, blocks, self->real_spectrum_size);

      if (is_valid_median) {
        set_noise_profile_available(self->noise_profile, noise_estimator_type);
      }
      break;
    }
    case MAX:
      (void)max_spectrum(noise_profile, signal_spectrum,
                         self->real_spectrum_size);
      set_noise_profile_available(self->noise_profile, noise_estimator_type);
      break;
    case MINIMUM:
      (void)min_spectrum(noise_profile, signal_spectrum,
                         self->real_spectrum_size);
      set_noise_profile_available(self->noise_profile, noise_estimator_type);
      break;

    default:
      break;
  }

  return true;
}

void noise_estimation_finalize(NoiseEstimator* self,
                               NoiseEstimatorType noise_estimator_type) {
  if (!self) {
    return;
  }

  float* noise_profile =
      get_noise_profile(self->noise_profile, noise_estimator_type);

  if (noise_profile && is_noise_estimation_available(self->noise_profile,
                                                     noise_estimator_type)) {
    // Basic refinement
    interpolate_spectrum_gaps(noise_profile, self->real_spectrum_size,
                              NOISE_ESTIMATION_INTERPOLATION_THRESHOLD);
    smooth_spectrum(noise_profile, self->real_spectrum_size,
                    NOISE_ESTIMATION_SMOOTHING_FACTOR);
  }
}
