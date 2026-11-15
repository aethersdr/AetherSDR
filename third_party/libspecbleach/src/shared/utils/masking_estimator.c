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

#include "masking_estimator.h"
#include "../configurations.h"
#include "shared/utils/absolute_hearing_thresholds.h"
#include "shared/utils/critical_bands.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Note: Psychoacoustic constants are now imported from configurations.h

/**
 * compute_spreading_gain: Level-dependent spectral spreading.
 * Uses a modified Schroeder spreading function where the upward slope
 * varies with level reflecting the broadening of masking at high intensities.
 */
static float compute_spreading_gain(float dz, float level_db);

/**
 * compute_tonality_factor: SFM-based NMT/TMN classification.
 * Uses Spectral Flatness Measure to differentiate between Tone-masking-Noise
 * and Noise-masking-Tone, adjusting the masking offset accordingly.
 */
static float compute_tonality_factor(MaskingEstimator* self,
                                     const float* spectrum, uint32_t band);

struct MaskingEstimator {
  uint32_t fft_size;
  uint32_t sample_rate;

  AbsoluteHearingThresholds* reference_spectrum;
  CriticalBands* critical_bands;
  CriticalBandIndexes band_indexes;

  uint32_t number_critical_bands;
  uint32_t real_spectrum_size;

  float* critical_bands_spectrum;
  float* critical_bands_reference_spectrum;
  float* spreading_matrix; // Matrix for simultaneous masking
  float* masking_offset;
  float* previous_thresholds;
  float* forward_decays;
  float spectral_additivity_exponent;
  bool use_temporal_masking;
  float backward_decay;
  float* future_thresholds;
  bool absolute_threshold_enabled;
  float* absolute_threshold_cb;

  // Temporary buffers to avoid VLAs and stack overflows
  float* future_cb_spectrum_buf;
  float* bark_levels_buf;
  float* spreaded_future_buf;
  float* spreaded_current_buf;
};

MaskingEstimator* masking_estimation_initialize(
    const uint32_t fft_size, const uint32_t sample_rate,
    CriticalBandType critical_band_type, SpectrumType spectrum_type,
    bool use_absolute_threshold, bool use_temporal_masking) {

  MaskingEstimator* self =
      (MaskingEstimator*)calloc(1U, sizeof(MaskingEstimator));

  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;
  self->sample_rate = sample_rate;

  self->critical_bands = critical_bands_initialize(
      self->sample_rate, self->fft_size, critical_band_type);
  if (!self->critical_bands) {
    masking_estimation_free(self);
    return NULL;
  }
  self->number_critical_bands =
      get_number_of_critical_bands(self->critical_bands);

  self->critical_bands_spectrum =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->critical_bands_reference_spectrum =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->spreading_matrix = (float*)calloc(
      (size_t)self->number_critical_bands * (size_t)self->number_critical_bands,
      sizeof(float));
  self->masking_offset =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->previous_thresholds =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->future_thresholds =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->forward_decays =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->absolute_threshold_cb =
      (float*)calloc(self->number_critical_bands, sizeof(float));

  self->future_cb_spectrum_buf =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->bark_levels_buf =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->spreaded_future_buf =
      (float*)calloc(self->number_critical_bands, sizeof(float));
  self->spreaded_current_buf =
      (float*)calloc(self->number_critical_bands, sizeof(float));

  self->reference_spectrum = absolute_hearing_thresholds_initialize(
      self->sample_rate, self->fft_size, spectrum_type);

  self->spectral_additivity_exponent = SPECTRAL_ADDITIVITY_EXPONENT_PEAQ;
  self->use_temporal_masking = use_temporal_masking;
  self->absolute_threshold_enabled = use_absolute_threshold;

  if (!self->critical_bands_spectrum ||
      !self->critical_bands_reference_spectrum || !self->spreading_matrix ||
      !self->masking_offset || !self->previous_thresholds ||
      !self->future_thresholds || !self->forward_decays ||
      !self->absolute_threshold_cb || !self->future_cb_spectrum_buf ||
      !self->bark_levels_buf || !self->spreaded_future_buf ||
      !self->spreaded_current_buf || !self->reference_spectrum) {
    masking_estimation_free(self);
    return NULL;
  }

  // Temporal masking decay constants
  const float hop_time = (float)fft_size / (4.0F * (float)sample_rate);

  // Frequency-dependent forward masking (Low: 100ms, High: 25ms)
  for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
    const float bark = fminf((float)j, 24.0F);
    const float weight = bark / 24.0F; // 0 to 1
    const float tau = ((1.0F - weight) * FORWARD_MASKING_TAU_LOW_MS) +
                      (weight * FORWARD_MASKING_TAU_HIGH_MS);
    self->forward_decays[j] = expf(-hop_time / tau);
  }

  // Backward masking (10ms) remains constant across frequency
  self->backward_decay = expf(-hop_time / BACKWARD_MASKING_TAU_MS);

  return self;
}

void masking_estimation_free(MaskingEstimator* self) {
  if (!self) {
    return;
  }
  absolute_hearing_thresholds_free(self->reference_spectrum);
  critical_bands_free(self->critical_bands);

  free(self->critical_bands_spectrum);
  free(self->critical_bands_reference_spectrum);
  free(self->spreading_matrix);
  free(self->masking_offset);
  free(self->previous_thresholds);
  free(self->future_thresholds);
  free(self->forward_decays);
  free(self->absolute_threshold_cb);
  free(self->future_cb_spectrum_buf);
  free(self->bark_levels_buf);
  free(self->spreaded_future_buf);
  free(self->spreaded_current_buf);

  free(self);
}

bool compute_masking_thresholds(MaskingEstimator* self, const float* spectrum,
                                const float* future_spectrum,
                                float* masking_thresholds) {
  if (!self || !spectrum || !masking_thresholds) {
    return false;
  }

  compute_critical_bands_spectrum(self->critical_bands, spectrum,
                                  self->critical_bands_spectrum);

  const float spectral_p = self->spectral_additivity_exponent;
  const float spectral_inv_p = 1.0F / spectral_p;

  // 1. Calculate spreaded future spectrum (Frequency Masking only)
  if (future_spectrum) {
    compute_critical_bands_spectrum(self->critical_bands, future_spectrum,
                                    self->future_cb_spectrum_buf);

    for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
      self->bark_levels_buf[j] =
          (10.F * log10f(self->future_cb_spectrum_buf[j] + SPECTRAL_EPSILON)) +
          DB_FS_TO_SPL_REF;
    }

    for (uint32_t i = 0U; i < self->number_critical_bands; i++) {
      float spreaded_p = 0.F;
      for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
        const float dz = (float)i - (float)j;
        const float gain = compute_spreading_gain(dz, self->bark_levels_buf[j]);
        spreaded_p += powf(self->future_cb_spectrum_buf[j] * gain, spectral_p);
      }
      self->spreaded_future_buf[i] = powf(spreaded_p, spectral_inv_p);
    }

    for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
      const float tonality_factor =
          compute_tonality_factor(self, future_spectrum, j);
      const float bark_idx = fminf((float)(j + 1), 25.0F);
      // Tone-masking-noise (TMN) has higher offset (~16-41dB depending on Bark)
      // Noise-masking-tone (NMT) has lower offset (~6dB)
      const float offset = (tonality_factor * (TMN_OFFSET_BASE + bark_idx)) +
                           (NMT_OFFSET_DB * (1.F - tonality_factor));

      self->future_thresholds[j] =
          powf(10.F, (log10f(self->spreaded_future_buf[j] + SPECTRAL_EPSILON) -
                      (offset / 10.F)));
    }
  }

  for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
    self->bark_levels_buf[j] =
        (10.F * log10f(self->critical_bands_spectrum[j] + SPECTRAL_EPSILON)) +
        DB_FS_TO_SPL_REF;
  }

  for (uint32_t i = 0U; i < self->number_critical_bands; i++) {
    float spreaded_p = 0.F;
    for (uint32_t j = 0U; j < self->number_critical_bands; j++) {
      const float dz = (float)i - (float)j;
      const float gain = compute_spreading_gain(dz, self->bark_levels_buf[j]);
      spreaded_p += powf(self->critical_bands_spectrum[j] * gain, spectral_p);
    }
    self->spreaded_current_buf[i] = powf(spreaded_p, spectral_inv_p);
  }

  for (uint32_t j = 0U; j < self->number_critical_bands; j++) {

    const float tonality_factor = compute_tonality_factor(self, spectrum, j);
    const float bark_idx = fminf((float)(j + 1), 25.0F);

    self->masking_offset[j] = (tonality_factor * (TMN_OFFSET_BASE + bark_idx)) +
                              (NMT_OFFSET_DB * (1.F - tonality_factor));

    // 1. Calculate frequency masking threshold for current frame
    float threshold =
        powf(10.F, (log10f(self->spreaded_current_buf[j] + SPECTRAL_EPSILON) -
                    (self->masking_offset[j] / 10.F)));

    // 2. Combine with temporal masking using Power Law (p=0.6)
    // Total_T = (T_freq^p + T_forward^p + T_backward^p)^(1/p)
    // This model (Johnston, 1988) better reflects the non-linear summation
    // of multiple maskers compared to simple linear addition.
    if (self->use_temporal_masking) {
      float threshold_p = powf(threshold, POWER_LAW_EXPONENT);

      // Add forward masking contribution
      float forward_threshold =
          self->previous_thresholds[j] * self->forward_decays[j];
      threshold_p += powf(forward_threshold, POWER_LAW_EXPONENT);

      // Add backward masking contribution if available
      if (future_spectrum) {
        float backward_threshold =
            self->future_thresholds[j] * self->backward_decay;
        threshold_p += powf(backward_threshold, POWER_LAW_EXPONENT);
      }

      threshold = powf(threshold_p, 1.0F / POWER_LAW_EXPONENT);
    }

    self->previous_thresholds[j] = threshold; // Update state for next frame

    self->band_indexes = get_band_indexes(self->critical_bands, j);

    for (uint32_t k = self->band_indexes.start_position;
         k < self->band_indexes.end_position; k++) {
      masking_thresholds[k] = threshold;
    }
  }

  if (self->absolute_threshold_enabled) {
    apply_thresholds_as_floor(self->reference_spectrum, masking_thresholds);
  }

  return true;
}

static float compute_spreading_gain(float dz, float level_db) {
  const float s_up =
      fminf(fmaxf(S_MAX_UPWARD - ((level_db - S_LEVEL_REF_DB) * S_SLOPE_FACTOR),
                  S_MIN_UPWARD),
            S_MAX_UPWARD);
  const float s_total = (S_DOWNWARD + s_up) / 2.F;
  const float s_offset = (S_DOWNWARD - s_up) / 2.F;

  const float y = dz + 0.474F;
  const float sf_db =
      15.81F + (s_offset * y) - (s_total * sqrtf(1.F + (y * y)));

  return powf(10.F, sf_db / 10.F);
}

static float compute_tonality_factor(MaskingEstimator* self,
                                     const float* spectrum, uint32_t band) {
  float sum_bins = 0.F;
  float sum_log_bins = 0.F;

  self->band_indexes = get_band_indexes(self->critical_bands, band);

  for (uint32_t k = self->band_indexes.start_position;
       k < self->band_indexes.end_position; k++) {
    const float val = fmaxf(spectrum[k], SPECTRAL_EPSILON);
    sum_bins += val;
    sum_log_bins += log10f(val);
  }

  float bins_in_band = (float)self->band_indexes.end_position -
                       (float)self->band_indexes.start_position;

  if (bins_in_band <= 1.0F) {
    return 1.0F;
  }

  const float sfm =
      10.F * ((sum_log_bins / bins_in_band) - log10f(sum_bins / bins_in_band));

  // SFM range is mapped to [0, 1] tonality factor
  const float tonality_factor =
      fminf(fmaxf((sfm - SFM_MAX_DB) / (SFM_MIN_DB - SFM_MAX_DB), 0.0F), 1.0F);

  return tonality_factor;
}
