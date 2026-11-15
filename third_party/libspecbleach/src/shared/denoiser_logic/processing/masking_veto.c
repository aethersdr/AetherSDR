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

#include "shared/denoiser_logic/processing/masking_veto.h"
#include "shared/configurations.h"
#include "shared/utils/critical_bands.h"
#include "shared/utils/masking_estimator.h"
#include "shared/utils/spectral_smoother.h"
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct MaskingVeto {
  uint32_t real_spectrum_size;
  MaskingEstimator* masking_estimator;
  CriticalBands* critical_bands_helper;
  float* clean_signal_estimation;
  float* stable_clean_signal;
  float* masking_thresholds;
  float* band_audibility;
  float* band_audibility_memory; // Smoothed state
  float* band_centers;           // Store center bin for each band
  uint32_t sample_rate;
  float* future_clean_estimation_buf;
  float* band_energies_buf;
};

MaskingVeto* masking_veto_initialize(uint32_t fft_size, uint32_t sample_rate,
                                     CriticalBandType critical_band_type,
                                     SpectrumType spectrum_type,
                                     bool use_absolute_threshold,
                                     bool use_temporal_masking) {

  MaskingVeto* self = (MaskingVeto*)calloc(1U, sizeof(MaskingVeto));
  if (!self) {
    return NULL;
  }

  self->real_spectrum_size = (fft_size / 2U) + 1U;
  self->masking_estimator = masking_estimation_initialize(
      fft_size, sample_rate, critical_band_type, spectrum_type,
      use_absolute_threshold, use_temporal_masking);

  // Helper to access band indexes for classification
  self->critical_bands_helper =
      critical_bands_initialize(sample_rate, fft_size, critical_band_type);

  if (!self->masking_estimator || !self->critical_bands_helper) {
    masking_veto_free(self);
    return NULL;
  }

  const uint32_t num_bands =
      get_number_of_critical_bands(self->critical_bands_helper);

  self->clean_signal_estimation =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->stable_clean_signal =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->masking_thresholds =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->band_audibility = (float*)calloc(num_bands, sizeof(float));
  self->band_audibility_memory = (float*)calloc(num_bands, sizeof(float));
  self->band_centers = (float*)calloc(num_bands, sizeof(float));
  self->future_clean_estimation_buf =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->band_energies_buf = (float*)calloc(num_bands, sizeof(float));
  self->sample_rate = sample_rate;

  if (!self->clean_signal_estimation || !self->stable_clean_signal ||
      !self->masking_thresholds || !self->band_audibility ||
      !self->band_audibility_memory || !self->band_centers ||
      !self->future_clean_estimation_buf || !self->band_energies_buf) {
    masking_veto_free(self);
    return NULL;
  }

  // Pre-calculate band center bins for interpolation
  for (uint32_t j = 0; j < num_bands; j++) {
    const CriticalBandIndexes indexes =
        get_band_indexes(self->critical_bands_helper, j);
    self->band_centers[j] =
        (float)indexes.start_position +
        ((float)(indexes.end_position - indexes.start_position) / 2.0F);
  }

  return self;
}

void masking_veto_free(MaskingVeto* self) {
  if (!self) {
    return;
  }

  masking_estimation_free(self->masking_estimator);
  critical_bands_free(self->critical_bands_helper);
  free(self->clean_signal_estimation);
  free(self->stable_clean_signal);
  free(self->masking_thresholds);
  free(self->band_audibility);
  free(self->band_audibility_memory);
  free(self->band_centers);
  free(self->future_clean_estimation_buf);
  free(self->band_energies_buf);
  free(self);
}

void masking_veto_apply(MaskingVeto* self, const float* smoothed_spectrum,
                        const float* noise_spectrum,
                        const float* future_spectrum, float* alpha,
                        float depth) {
  if (!self || !smoothed_spectrum || !noise_spectrum || !alpha ||
      depth < 0.0F || self->real_spectrum_size == 0U) {
    return;
  }

  // 1. Estimate clean signal magnitude from SMOOTHED signal
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    const float current_clean =
        fmaxf(smoothed_spectrum[k] - noise_spectrum[k], 0.0F);

    // Stabilize the clean signal estimation (prevents gargle in spreading
    // skirts)
    self->stable_clean_signal[k] =
        (MASKING_VETO_SMOOTHING * current_clean) +
        ((1.0F - MASKING_VETO_SMOOTHING) * self->stable_clean_signal[k]);
    self->clean_signal_estimation[k] = self->stable_clean_signal[k];
  }

  // 1.1 Estimate future clean signal magnitude for backward masking
  float* future_clean_estimation = NULL;
  if (future_spectrum) {
    for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
      self->future_clean_estimation_buf[k] =
          fmaxf(future_spectrum[k] - noise_spectrum[k], 0.0F);
    }
    future_clean_estimation = self->future_clean_estimation_buf;
  }

  // 2. Compute psychoacoustic masking thresholds
  if (!compute_masking_thresholds(
          self->masking_estimator, self->clean_signal_estimation,
          future_clean_estimation, self->masking_thresholds)) {
    return;
  }

  /**
   * 3. Calculate Band-wise Protection from NMR
   *
   * NMR (Noise-to-Mask Ratio) determines whether noise is audible:
   *   NMR <= 0dB: noise is below masking threshold -> inaudible -> protect
   *   NMR >= NMR_RANGE: noise is clearly audible -> no protection
   *
   * Protection is the inverse of audibility: 1.0 = full protection, 0.0 = none
   */
  const uint32_t num_bands =
      get_number_of_critical_bands(self->critical_bands_helper);

  for (uint32_t j = 0U; j < num_bands; j++) {
    const CriticalBandIndexes indexes =
        get_band_indexes(self->critical_bands_helper, j);

    const uint32_t bins_in_band = indexes.end_position - indexes.start_position;
    if (bins_in_band > 0) {
      float band_noise_energy = 0.0F;
      float band_threshold = 0.0F;

      for (uint32_t k = indexes.start_position; k < indexes.end_position; k++) {
        band_noise_energy += noise_spectrum[k];
        band_threshold += self->masking_thresholds[k];
      }

      const float nmr_db =
          10.0F * log10f((band_noise_energy + SPECTRAL_EPSILON) /
                         (band_threshold + SPECTRAL_EPSILON));

      // Map NMR to protection (inverse of audibility):
      //   NMR <= 0dB -> protection = 1.0 (noise masked, preserve energy)
      //   NMR >= NMR_RANGE -> protection = 0.0 (noise audible, allow
      //   suppression)
      float protection;
      if (nmr_db <= 0.0F) {
        protection = 1.0F;
      } else if (nmr_db >= MASKING_VETO_NMR_RANGE) {
        protection = 0.0F;
      } else {
        protection = 1.0F - (nmr_db / MASKING_VETO_NMR_RANGE);
      }

      self->band_audibility[j] = protection;
    } else {
      self->band_audibility[j] = 0.0F;
    }
  }

  // Temporal Stabilization:
  // Smooth the protection metric to prevent rapid switching
  spectral_smoothing_apply_simple_temporal(self->band_audibility,
                                           self->band_audibility_memory,
                                           num_bands, MASKING_VETO_SMOOTHING);

  // Spectral Stabilization:
  // Smooth protection ACROSS bands to prevent sharp spectral edges.
  spectral_smoothing_apply_spatial(self->band_audibility, num_bands);

  /**
   * 4. Apply Interpolated Veto
   * Interpolate protection for each bin between band centers.
   * This eliminates sharp gain transitions at Bark band boundaries.
   */
  uint32_t current_band = 0;
  for (uint32_t k = 0; k < self->real_spectrum_size; k++) {
    while (current_band < num_bands - 1 &&
           (float)k > self->band_centers[current_band + 1]) {
      current_band++;
    }

    float bin_protection;
    if (current_band < num_bands - 1) {
      const float x0 = self->band_centers[current_band];
      const float x1 = self->band_centers[current_band + 1];
      const float y0 = self->band_audibility[current_band];
      const float y1 = self->band_audibility[current_band + 1];

      if (x1 > x0) {
        const float t = ((float)k - x0) / (x1 - x0);
        const float t_clamped = fmaxf(0.0F, fminf(1.0F, t));
        bin_protection = (y0 * (1.0F - t_clamped)) + (y1 * t_clamped);
      } else {
        bin_protection = y0;
      }
    } else {
      bin_protection = self->band_audibility[num_bands - 1];
    }

    // Lerp alpha toward 1.0 (no extra subtraction) based on protection.
    // At full protection (depth=1.0, protection=1.0): alpha -> 1.0
    // At zero protection: alpha unchanged
    const float veto_amount = bin_protection * depth;
    alpha[k] = alpha[k] + ((1.0F - alpha[k]) * veto_amount);
  }
}
