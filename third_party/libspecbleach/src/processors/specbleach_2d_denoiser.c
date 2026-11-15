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

#include "specbleach_2d_denoiser.h"
#include "denoiser2d/spectral_2d_denoiser.h"
#include "shared/configurations.h"
#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/stft/stft_processor.h"
#include "shared/utils/general_utils.h"
#include <stdlib.h>
#include <string.h>

typedef struct Sb2DDenoiser {
  uint32_t sample_rate;
  uint32_t hop;
  Denoiser2DParameters denoise_parameters;

  NoiseProfile* noise_profile;
  SpectralProcessorHandle spectral_2d_denoiser;
  StftProcessor* stft_processor;
} Sb2DDenoiser;

SpectralBleachHandle specbleach_2d_initialize(const uint32_t sample_rate,
                                              float frame_size) {
  if (sample_rate == 0 || frame_size <= 0.0f) {
    return NULL;
  }

  Sb2DDenoiser* self = (Sb2DDenoiser*)calloc(1U, sizeof(Sb2DDenoiser));
  if (!self) {
    return NULL;
  }

  self->sample_rate = sample_rate;

  self->stft_processor = stft_processor_initialize(
      sample_rate, frame_size, OVERLAP_FACTOR_2D, PADDING_CONFIGURATION_2D,
      ZEROPADDING_AMOUNT_2D, INPUT_WINDOW_TYPE_2D, OUTPUT_WINDOW_TYPE_2D);

  if (!self->stft_processor) {
    specbleach_2d_free(self);
    return NULL;
  }

  const uint32_t fft_size = get_stft_fft_size(self->stft_processor);
  self->hop = fft_size / OVERLAP_FACTOR_2D;

  self->noise_profile = noise_profile_initialize(fft_size);
  if (!self->noise_profile) {
    specbleach_2d_free(self);
    return NULL;
  }

  self->spectral_2d_denoiser = spectral_2d_denoiser_initialize(
      sample_rate, fft_size, OVERLAP_FACTOR_2D, self->noise_profile);

  if (!self->spectral_2d_denoiser) {
    specbleach_2d_free(self);
    return NULL;
  }

  return self;
}

void specbleach_2d_free(SpectralBleachHandle instance) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self) {
    return;
  }

  if (self->spectral_2d_denoiser) {
    spectral_2d_denoiser_free(self->spectral_2d_denoiser);
  }
  if (self->noise_profile) {
    noise_profile_free(self->noise_profile);
  }
  if (self->stft_processor) {
    stft_processor_free(self->stft_processor);
  }

  free(self);
}

uint32_t specbleach_2d_get_latency(SpectralBleachHandle instance) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self) {
    return 0;
  }

  // Base STFT latency
  uint32_t stft_latency = get_stft_latency(self->stft_processor);

  // Additional NLM look-ahead latency
  uint32_t nlm_latency_frames =
      spectral_2d_denoiser_get_latency_frames(self->spectral_2d_denoiser);
  uint32_t nlm_latency_samples = nlm_latency_frames * self->hop;

  return stft_latency + nlm_latency_samples;
}

bool specbleach_2d_process(SpectralBleachHandle instance,
                           const uint32_t number_of_samples, const float* input,
                           float* output) {
  if (!instance || number_of_samples == 0 || !input || !output) {
    return false;
  }

  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  stft_processor_run(self->stft_processor, number_of_samples, input, output,
                     &spectral_2d_denoiser_run, self->spectral_2d_denoiser);

  return true;
}

uint32_t specbleach_2d_get_noise_profile_size(SpectralBleachHandle instance) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return 0;
  }

  return get_noise_profile_size(self->noise_profile);
}

bool specbleach_2d_load_noise_profile_for_mode(SpectralBleachHandle instance,
                                               const float* restored_profile,
                                               const uint32_t profile_size,
                                               const uint32_t block_count,
                                               const int mode) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile || !restored_profile || mode < 1 ||
      mode > 4) {
    return false;
  }

  if (profile_size != get_noise_profile_size(self->noise_profile)) {
    return false;
  }

  set_noise_profile(self->noise_profile, mode, restored_profile, profile_size,
                    block_count);

  return true;
}

bool specbleach_2d_reset_noise_profile(SpectralBleachHandle instance) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return false;
  }

  reset_noise_profile(self->noise_profile);
  return true;
}

uint32_t specbleach_2d_get_noise_profile_block_count_for_mode(
    SpectralBleachHandle instance, int mode) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return 0;
  }

  if (mode < 1 || mode > 4) {
    return 0;
  }

  return get_noise_profile_block_count(self->noise_profile, mode);
}

float* specbleach_2d_get_noise_profile_for_mode(SpectralBleachHandle instance,
                                                int mode) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return NULL;
  }

  if (mode < 1 || mode > 4) {
    return NULL;
  }

  return get_noise_profile(self->noise_profile, mode);
}

bool specbleach_2d_noise_profile_available_for_mode(
    SpectralBleachHandle instance, int mode) {
  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  if (!self || !self->noise_profile) {
    return false;
  }

  if (mode < 1 || mode > 4) {
    return false;
  }

  return is_noise_estimation_available(self->noise_profile, mode);
}

bool specbleach_2d_load_parameters(
    SpectralBleachHandle instance,
    SpectralBleach2DDenoiserParameters parameters) {
  if (!instance) {
    return false;
  }

  Sb2DDenoiser* self = (Sb2DDenoiser*)instance;

  // Convert public parameters to internal format
  // clang-format off
  self->denoise_parameters = (Denoiser2DParameters){
      .learn_noise = parameters.learn_noise,
      .residual_listen = parameters.residual_listen,
      .reduction_amount =
          from_db_to_coefficient(parameters.reduction_amount * -1.F),
      .smoothing_factor = parameters.smoothing_factor,
      .whitening_factor = parameters.whitening_factor / 100.F,
      .adaptive_noise = parameters.adaptive_noise,
      .noise_estimation_method = parameters.noise_estimation_method,
      .nlm_masking_protection = parameters.nlm_masking_protection,
      .suppression_strength = parameters.suppression_strength / 100.F,
      .aggressiveness = parameters.aggressiveness,
      .tonal_reduction =
          from_db_to_coefficient(parameters.tonal_reduction * -1.F),
  };
  // clang-format on

  load_2d_reduction_parameters(self->spectral_2d_denoiser,
                               self->denoise_parameters);

  return true;
}
