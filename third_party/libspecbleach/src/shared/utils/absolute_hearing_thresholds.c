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

#include "absolute_hearing_thresholds.h"
#include "../configurations.h"
#include "../stft/fft_transform.h"
#include "spectral_utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void generate_sinewave(AbsoluteHearingThresholds* self);
static void compute_spl_reference_spectrum(AbsoluteHearingThresholds* self);
static void compute_absolute_thresholds(AbsoluteHearingThresholds* self);

struct AbsoluteHearingThresholds {
  float* sinewave;
  float* window;
  float* absolute_thresholds;
  float spl_reference_value;

  SpectralFeatures* spectral_features;
  FftTransform* fft_transform;

  SpectrumType spectrum_type;
  uint32_t fft_size;
  uint32_t real_spectrum_size;
  uint32_t sample_rate;
  float sine_wave_amplitude;
  float sine_wave_frequency;
  float reference_level;
};

AbsoluteHearingThresholds* absolute_hearing_thresholds_initialize(
    const uint32_t sample_rate, const uint32_t fft_size,
    SpectrumType spectrum_type) {
  AbsoluteHearingThresholds* self =
      (AbsoluteHearingThresholds*)calloc(1U, sizeof(AbsoluteHearingThresholds));

  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;
  self->sample_rate = sample_rate;
  self->spectrum_type = spectrum_type;
  self->sine_wave_amplitude = SINE_AMPLITUDE;
  self->sine_wave_frequency = REFERENCE_SINE_WAVE_FREQ;
  self->reference_level = REFERENCE_LEVEL;

  self->fft_transform = fft_transform_initialize_bins(self->fft_size);

  self->absolute_thresholds =
      (float*)calloc(self->real_spectrum_size, sizeof(float));

  self->sinewave = (float*)calloc(self->fft_size, sizeof(float));
  self->window = (float*)calloc(self->fft_size, sizeof(float));

  self->spectral_features =
      spectral_features_initialize(self->real_spectrum_size);

  if (!self->fft_transform || !self->absolute_thresholds || !self->sinewave ||
      !self->window || !self->spectral_features) {
    absolute_hearing_thresholds_free(self);
    return NULL;
  }

  generate_sinewave(self);
  (void)get_fft_window(self->window, self->fft_size, VORBIS_WINDOW);
  compute_spl_reference_spectrum(self);
  compute_absolute_thresholds(self);

  return self;
}

void absolute_hearing_thresholds_free(AbsoluteHearingThresholds* self) {
  if (!self) {
    return;
  }
  fft_transform_free(self->fft_transform);
  spectral_features_free(self->spectral_features);

  free(self->sinewave);
  free(self->window);

  free(self->absolute_thresholds);

  free(self);
}

static void generate_sinewave(AbsoluteHearingThresholds* self) {
  for (uint32_t k = 0U; k < self->fft_size; k++) {
    self->sinewave[k] =
        self->sine_wave_amplitude *
        sinf((2.F * M_PIf * (float)k * self->sine_wave_frequency) /
             (float)self->sample_rate);
  }
}

static void compute_spl_reference_spectrum(AbsoluteHearingThresholds* self) {
  for (uint32_t k = 0U; k < self->fft_size; k++) {
    get_fft_input_buffer(self->fft_transform)[k] =
        self->sinewave[k] * self->window[k];
  }

  compute_forward_fft(self->fft_transform);

  float* reference_spectrum = get_spectral_feature(
      self->spectral_features, get_fft_output_buffer(self->fft_transform),
      self->fft_size, self->spectrum_type);

  float max_val = 0.F;
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    if (reference_spectrum[k] > max_val) {
      max_val = reference_spectrum[k];
    }
  }

  self->spl_reference_value =
      self->reference_level - (10.F * log10f(max_val + SPECTRAL_EPSILON));
}

bool apply_thresholds_as_floor(AbsoluteHearingThresholds* self,
                               float* spectrum) {
  if (!self || !spectrum) {
    return false;
  }

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    const float spl_level = (10.F * log10f(spectrum[k] + SPECTRAL_EPSILON)) +
                            self->spl_reference_value;
    spectrum[k] =
        powf(10.F, fmaxf(spl_level, self->absolute_thresholds[k]) / 10.F);
  }

  return true;
}

static void compute_absolute_thresholds(AbsoluteHearingThresholds* self) {
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    const float frequency =
        fmaxf(fft_bin_to_freq(k, self->sample_rate, self->fft_size), 20.F);
    self->absolute_thresholds[k] =
        (3.64F * powf((frequency / 1000.F), -0.8F)) -
        (6.5F * expf(-0.6F * powf(((frequency / 1000.F) - 3.3F), 2.F))) +
        (powf(10.F, -3.F) * powf((frequency / 1000.F), 4.F));
  }
}
