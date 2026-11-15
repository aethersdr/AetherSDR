/*
libspecbleach - A spectral processing library

Copyright 2022-2026 Luciano Dato <lucianodato@gmail.com>

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

#include "shared/denoiser_logic/processing/suppression_engine.h"
#include "shared/configurations.h"
#include "shared/utils/critical_bands.h"
#include "shared/utils/masking_estimator.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

struct SuppressionEngine {
  uint32_t real_spectrum_size;
  uint32_t sample_rate;

  CriticalBands* critical_bands;
  MaskingEstimator* masking_estimation;

  float* masking_thresholds;
  float* clean_signal_estimation;
  float* critical_bands_noise_profile;
  float* critical_bands_reference_spectrum;

  uint32_t number_critical_bands;
};

SuppressionEngine* suppression_engine_initialize(
    uint32_t real_spectrum_size, uint32_t sample_rate,
    CriticalBandType critical_band_type, SpectrumType spectrum_type,
    bool use_absolute_threshold, bool use_temporal_masking) {

  SuppressionEngine* self =
      (SuppressionEngine*)calloc(1U, sizeof(SuppressionEngine));
  if (!self) {
    return NULL;
  }

  self->real_spectrum_size = real_spectrum_size;
  self->sample_rate = sample_rate;
  uint32_t fft_size = (real_spectrum_size - 1U) * 2U;

  self->critical_bands =
      critical_bands_initialize(sample_rate, fft_size, critical_band_type);
  self->masking_estimation = masking_estimation_initialize(
      fft_size, sample_rate, critical_band_type, spectrum_type,
      use_absolute_threshold, use_temporal_masking);

  if (!self->critical_bands || !self->masking_estimation) {
    suppression_engine_free(self);
    return NULL;
  }

  self->number_critical_bands =
      get_number_of_critical_bands(self->critical_bands);

  self->critical_bands_noise_profile =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->critical_bands_reference_spectrum =
      (float*)calloc(self->number_critical_bands, sizeof(float));

  self->masking_thresholds = (float*)calloc(real_spectrum_size, sizeof(float));
  self->clean_signal_estimation =
      (float*)calloc(real_spectrum_size, sizeof(float));

  if (!self->critical_bands_noise_profile ||
      !self->critical_bands_reference_spectrum || !self->masking_thresholds ||
      !self->clean_signal_estimation) {
    suppression_engine_free(self);
    return NULL;
  }

  return self;
}

void suppression_engine_free(SuppressionEngine* self) {
  if (!self) {
    return;
  }

  critical_bands_free(self->critical_bands);
  masking_estimation_free(self->masking_estimation);

  free(self->critical_bands_noise_profile);
  free(self->critical_bands_reference_spectrum);
  free(self->masking_thresholds);
  free(self->clean_signal_estimation);

  free(self);
}

static void calculate_berouti_per_bin(SuppressionEngine* self,
                                      const float* reference_spectrum,
                                      const float* noise_spectrum,
                                      float strength, float* alpha,
                                      float* beta) {
  const float alpha_max_user = ALPHA_MIN + (strength * (ALPHA_MAX - ALPHA_MIN));
  const float snr_range_db =
      SUPPRESSION_HIGHER_SNR_DB - SUPPRESSION_LOWER_SNR_DB;

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    const float snr_db = 10.0F * log10f(reference_spectrum[k] /
                                        (noise_spectrum[k] + SPECTRAL_EPSILON));

    if (snr_db <= SUPPRESSION_LOWER_SNR_DB) {
      alpha[k] = alpha_max_user;
    } else if (snr_db >= SUPPRESSION_HIGHER_SNR_DB) {
      alpha[k] = ALPHA_MIN;
    } else {
      const float normalized_snr =
          (snr_db - SUPPRESSION_LOWER_SNR_DB) / snr_range_db;
      alpha[k] =
          alpha_max_user - (normalized_snr * (alpha_max_user - ALPHA_MIN));
    }
    beta[k] = 0.0F;
  }
}

static void calculate_global_snr(SuppressionEngine* self, const float* spectrum,
                                 const float* noise_spectrum,
                                 SuppressionParameters parameters, float* alpha,
                                 float* beta) {
  float noisy_spectrum_sum = 0.F;
  float noise_spectrum_sum = 0.F;

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    noisy_spectrum_sum += spectrum[k];
    noise_spectrum_sum += noise_spectrum[k];
  }

  const float snr_db = 10.F * log10f(noisy_spectrum_sum /
                                     (noise_spectrum_sum + SPECTRAL_EPSILON));

  float oversubtraction_factor;

  if (snr_db <= 0.F) {
    oversubtraction_factor = parameters.strength;
  } else if (snr_db >= 20.F) {
    oversubtraction_factor = ALPHA_MIN;
  } else {
    const float normalized_snr = snr_db / 20.F;
    oversubtraction_factor = ((1.F - normalized_snr) * parameters.strength) +
                             (normalized_snr * ALPHA_MIN);
  }

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    alpha[k] = oversubtraction_factor;
    beta[k] = parameters.undersubtraction;
  }
}

static void calculate_critical_bands_snr(SuppressionEngine* self,
                                         const float* spectrum,
                                         const float* noise_spectrum,
                                         SuppressionParameters parameters,
                                         float* alpha, float* beta) {
  compute_critical_bands_spectrum(self->critical_bands, noise_spectrum,
                                  self->critical_bands_noise_profile);
  compute_critical_bands_spectrum(self->critical_bands, spectrum,
                                  self->critical_bands_reference_spectrum);

  for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
    const CriticalBandIndexes band_indexes =
        get_band_indexes(self->critical_bands, j);

    const float snr_db =
        10.F *
        log10f(self->critical_bands_reference_spectrum[j] /
               (self->critical_bands_noise_profile[j] + SPECTRAL_EPSILON));

    float oversubtraction_factor;

    if (snr_db <= 0.F) {
      oversubtraction_factor = parameters.strength;
    } else if (snr_db >= 20.F) {
      oversubtraction_factor = ALPHA_MIN;
    } else {
      const float normalized_snr = snr_db / 20.F;
      oversubtraction_factor = ((1.F - normalized_snr) * parameters.strength) +
                               (normalized_snr * ALPHA_MIN);
    }

    for (uint32_t k = band_indexes.start_position;
         k < band_indexes.end_position; k++) {
      alpha[k] = oversubtraction_factor;
      beta[k] = parameters.undersubtraction;
    }
  }
}

static void calculate_masking_thresholds(SuppressionEngine* self,
                                         const float* spectrum,
                                         const float* noise_spectrum,
                                         SuppressionParameters parameters,
                                         float* alpha, float* beta) {
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    self->clean_signal_estimation[k] =
        fmaxf(spectrum[k] - noise_spectrum[k], 0.F);
  }

  compute_masking_thresholds(self->masking_estimation,
                             self->clean_signal_estimation, NULL,
                             self->masking_thresholds);

  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    const float nmr_db =
        10.F * log10f(noise_spectrum[k] /
                      (self->masking_thresholds[k] + SPECTRAL_EPSILON));

    if (nmr_db <= 0.F) {
      alpha[k] = ALPHA_MIN +
                 ((parameters.strength - ALPHA_MIN) * 0.1F); // Conservative
      beta[k] = 0.0f;
    } else if (nmr_db >= 20.F) {
      alpha[k] = parameters.strength;
      beta[k] = parameters.undersubtraction;
    } else {
      const float normalized_nmr = nmr_db / 20.F;
      alpha[k] = ((1.F - normalized_nmr) * ALPHA_MIN) +
                 (normalized_nmr * parameters.strength);
      beta[k] = ((1.F - normalized_nmr) * 0.0f) +
                (normalized_nmr * parameters.undersubtraction);
    }
  }
}

void suppression_engine_calculate(SuppressionEngine* self,
                                  const float* reference_spectrum,
                                  const float* noise_spectrum,
                                  SuppressionParameters parameters,
                                  float* alpha, float* beta) {
  if (!self || !reference_spectrum || !noise_spectrum || !alpha || !beta) {
    return;
  }

  switch (parameters.type) {
    case SUPPRESSION_BEROUTI_PER_BIN:
      calculate_berouti_per_bin(self, reference_spectrum, noise_spectrum,
                                parameters.strength, alpha, beta);
      break;
    case SUPPRESSION_GLOBAL_SNR:
      calculate_global_snr(self, reference_spectrum, noise_spectrum, parameters,
                           alpha, beta);
      break;
    case SUPPRESSION_CRITICAL_BANDS_SNR:
      calculate_critical_bands_snr(self, reference_spectrum, noise_spectrum,
                                   parameters, alpha, beta);
      break;
    case SUPPRESSION_MASKING_THRESHOLDS:
      calculate_masking_thresholds(self, reference_spectrum, noise_spectrum,
                                   parameters, alpha, beta);
      break;
    case SUPPRESSION_NONE:
      for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
        alpha[k] = ALPHA_MIN;
        beta[k] = 0.0F;
      }
      break;
  }
}
