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

#include "specbleach_denoiser.h"
#include "denoiser/spectral_denoiser.h"
#include "shared/configurations.h"
#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/stft/stft_processor.h"
#include "shared/utils/general_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct SbSpectralDenoiser {
  uint32_t sample_rate;
  DenoiserParameters denoise_parameters;

  NoiseProfile* noise_profile;
  SpectralProcessorHandle spectral_denoiser;
  StftProcessor* stft_processor;
} SbSpectralDenoiser;

SpectralBleachHandle specbleach_initialize(const uint32_t sample_rate,
                                           float frame_size) {
  if (sample_rate < 4000 || sample_rate > 192000 || frame_size <= 0.0f) {
    return NULL;
  }

  SbSpectralDenoiser* self =
      (SbSpectralDenoiser*)calloc(1U, sizeof(SbSpectralDenoiser));
  if (!self) {
    return NULL;
  }

  self->sample_rate = sample_rate;

  self->stft_processor = stft_processor_initialize(
      sample_rate, frame_size, OVERLAP_FACTOR_1D, PADDING_CONFIGURATION_1D,
      ZEROPADDING_AMOUNT_1D, INPUT_WINDOW_TYPE_1D, OUTPUT_WINDOW_TYPE_1D);

  if (!self->stft_processor) {
    specbleach_free(self);
    return NULL;
  }

  const uint32_t fft_size = get_stft_fft_size(self->stft_processor);
  const uint32_t real_spectrum_size =
      get_stft_real_spectrum_size(self->stft_processor);

  self->noise_profile = noise_profile_initialize(real_spectrum_size);

  if (!self->noise_profile) {
    specbleach_free(self);
    return NULL;
  }

  self->spectral_denoiser = spectral_denoiser_initialize(
      self->sample_rate, fft_size, OVERLAP_FACTOR_1D, self->noise_profile);

  if (!self->spectral_denoiser) {
    specbleach_free(self);
    return NULL;
  }

  return self;
}

void specbleach_free(SpectralBleachHandle instance) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  if (!self) {
    return;
  }

  if (self->noise_profile) {
    noise_profile_free(self->noise_profile);
  }
  if (self->spectral_denoiser) {
    spectral_denoiser_free(self->spectral_denoiser);
  }
  if (self->stft_processor) {
    stft_processor_free(self->stft_processor);
  }

  free(self);
}

uint32_t specbleach_get_latency(SpectralBleachHandle instance) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  if (!self || !self->stft_processor) {
    return 0;
  }

  return get_stft_latency(self->stft_processor);
}

bool specbleach_process(SpectralBleachHandle instance,
                        const uint32_t number_of_samples, const float* input,
                        float* output) {
  if (!instance || number_of_samples == 0 || !input || !output) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  stft_processor_run(self->stft_processor, number_of_samples, input, output,
                     &spectral_denoiser_run, self->spectral_denoiser);

  return true;
}

uint32_t specbleach_get_noise_profile_size(SpectralBleachHandle instance) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return 0;
  }

  return get_noise_profile_size(self->noise_profile);
}

bool specbleach_load_noise_profile_for_mode(SpectralBleachHandle instance,
                                            const float* restored_profile,
                                            const uint32_t profile_size,
                                            const uint32_t block_count,
                                            const int mode) {
  if (!instance || !restored_profile || mode < 1 || mode > 4) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  if (profile_size != get_noise_profile_size(self->noise_profile)) {
    return false;
  }

  set_noise_profile(self->noise_profile, mode, restored_profile, profile_size,
                    block_count);

  return true;
}

bool specbleach_reset_noise_profile(SpectralBleachHandle instance) {
  if (!instance) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  reset_noise_profile(self->noise_profile);

  return true;
}

uint32_t specbleach_get_noise_profile_block_count_for_mode(
    SpectralBleachHandle instance, int mode) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;
  if (!self || mode < 1 || mode > 4) {
    return 0;
  }
  return get_noise_profile_block_count(self->noise_profile, mode);
}

float* specbleach_get_noise_profile_for_mode(SpectralBleachHandle instance,
                                             int mode) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;
  if (!self || mode < 1 || mode > 4) {
    return NULL;
  }
  return get_noise_profile(self->noise_profile, mode);
}

bool specbleach_noise_profile_available_for_mode(SpectralBleachHandle instance,
                                                 int mode) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;
  if (!self || mode < 1 || mode > 4) {
    return false;
  }
  return is_noise_estimation_available(self->noise_profile, mode);
}

bool specbleach_load_parameters(SpectralBleachHandle instance,
                                SpectralBleachDenoiserParameters parameters) {
  if (!instance) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  // clang-format off
  self->denoise_parameters = (DenoiserParameters){
      .learn_noise = parameters.learn_noise,
      .residual_listen = parameters.residual_listen,
      .reduction_amount =
          from_db_to_coefficient(parameters.reduction_amount * -1.F),
      .smoothing_factor =
          remap_percentage_log_like_unity(parameters.smoothing_factor / 100.F),
      .whitening_factor = parameters.whitening_factor / 100.F,
      .adaptive_noise = parameters.adaptive_noise,
      .noise_estimation_method = parameters.noise_estimation_method,
      .masking_depth = parameters.masking_depth,
      .suppression_strength = parameters.suppression_strength / 100.F,
      .aggressiveness = parameters.aggressiveness,
      .tonal_reduction =
          from_db_to_coefficient(parameters.tonal_reduction * -1.F),
  };
  // clang-format on

  load_reduction_parameters(self->spectral_denoiser, self->denoise_parameters);

  return true;
}
