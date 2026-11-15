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

#include "spectral_smoother.h"
#include "critical_bands.h"
#include "transient_detector.h"
#include <stdlib.h>
#include <string.h>

static void spectrum_time_smoothing(SpectralSmoother* self, float smoothing);
static void spectrum_transient_aware_time_smoothing(SpectralSmoother* self,
                                                    float smoothing,
                                                    const float* spectrum);

struct SpectralSmoother {
  uint32_t fft_size;
  uint32_t real_spectrum_size;
  float adaptive_coefficient;
  float previous_adaptive_coefficient;
  TimeSmoothingType type;

  float* noise_spectrum;
  float* smoothed_spectrum;
  float* smoothed_spectrum_previous;

  CriticalBands* critical_bands;
  float* band_energies;
  float* onset_weights;
  TransientDetector* transient_detection;
};

SpectralSmoother* spectral_smoothing_initialize(const uint32_t fft_size,
                                                const uint32_t sample_rate,
                                                TimeSmoothingType type) {
  SpectralSmoother* self =
      (SpectralSmoother*)calloc(1U, sizeof(SpectralSmoother));

  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;
  self->type = type;
  self->previous_adaptive_coefficient = 0.F;
  self->adaptive_coefficient = 0.F;

  self->noise_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->smoothed_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  self->smoothed_spectrum_previous =
      (float*)calloc(self->real_spectrum_size, sizeof(float));

  self->critical_bands =
      critical_bands_initialize(sample_rate, self->fft_size, BARK_SCALE);
  if (!self->critical_bands) {
    spectral_smoothing_free(self);
    return NULL;
  }

  const uint32_t num_bands = get_number_of_critical_bands(self->critical_bands);
  self->band_energies = (float*)calloc(num_bands, sizeof(float));
  self->onset_weights = (float*)calloc(num_bands, sizeof(float));

  self->transient_detection = transient_detector_initialize(num_bands);

  if (!self->noise_spectrum || !self->smoothed_spectrum ||
      !self->smoothed_spectrum_previous || !self->band_energies ||
      !self->onset_weights || !self->transient_detection) {
    spectral_smoothing_free(self);
    return NULL;
  }

  return self;
}

void spectral_smoothing_free(SpectralSmoother* self) {
  if (!self) {
    return;
  }
  critical_bands_free(self->critical_bands);
  transient_detector_free(self->transient_detection);

  free(self->noise_spectrum);
  free(self->smoothed_spectrum);
  free(self->smoothed_spectrum_previous);
  free(self->band_energies);
  free(self->onset_weights);

  free(self);
}

bool spectral_smoothing_run(SpectralSmoother* self,
                            TimeSmoothingParameters parameters,
                            float* signal_spectrum) {
  if (!self || !signal_spectrum) {
    return false;
  }

  memcpy(self->smoothed_spectrum, signal_spectrum,
         sizeof(float) * self->real_spectrum_size);

  switch (self->type) {
    case FIXED:
      spectrum_time_smoothing(self, parameters.smoothing);
      break;
    case TRANSIENT_AWARE:
      spectrum_transient_aware_time_smoothing(self, parameters.smoothing,
                                              signal_spectrum);
      break;
    default:
      break;
  }

  memcpy(self->smoothed_spectrum_previous, self->smoothed_spectrum,
         sizeof(float) * self->real_spectrum_size);
  memcpy(signal_spectrum, self->smoothed_spectrum,
         sizeof(float) * self->real_spectrum_size);

  return true;
}

static void spectrum_transient_aware_time_smoothing(SpectralSmoother* self,
                                                    const float smoothing,
                                                    const float* spectrum) {
  // Calculate band energies for the transient detector
  const uint32_t num_bands = get_number_of_critical_bands(self->critical_bands);
  for (uint32_t j = 0U; j < num_bands; j++) {
    const CriticalBandIndexes indexes =
        get_band_indexes(self->critical_bands, j);
    float energy = 0.0F;
    for (uint32_t k = indexes.start_position; k < indexes.end_position; k++) {
      energy += spectrum[k];
    }
    self->band_energies[j] = energy;
  }

  // Retrieve onset weights (transient detection)
  // Note: We ignore the return value (global transient bool) because we process
  // per-band
  transient_detector_process(self->transient_detection, self->band_energies,
                             self->onset_weights);

  // Apply smoothing with per-band transient awareness
  for (uint32_t j = 0U; j < num_bands; j++) {
    const CriticalBandIndexes indexes =
        get_band_indexes(self->critical_bands, j);

    // If a transient is detected in this band (weight > 0), we reduce smoothing
    // weight 0.0 -> full smoothing
    // weight 1.0 -> no smoothing (track signal instantly)
    const float weight = self->onset_weights[j];
    const float effective_smoothing = smoothing * (1.0F - weight);

    for (uint32_t k = indexes.start_position; k < indexes.end_position; k++) {
      if (self->smoothed_spectrum[k] > self->smoothed_spectrum_previous[k]) {
        self->smoothed_spectrum[k] =
            (effective_smoothing * self->smoothed_spectrum_previous[k]) +
            ((1.F - effective_smoothing) * self->smoothed_spectrum[k]);
      }
    }
  }
}

static void spectrum_time_smoothing(SpectralSmoother* self,
                                    const float smoothing) {
  for (uint32_t k = 0U; k < self->real_spectrum_size; k++) {
    if (self->smoothed_spectrum[k] > self->smoothed_spectrum_previous[k]) {
      self->smoothed_spectrum[k] =
          (smoothing * self->smoothed_spectrum_previous[k]) +
          ((1.F - smoothing) * self->smoothed_spectrum[k]);
    }
  }
}

void spectral_smoothing_apply_spatial(float* data, uint32_t size) {
  if (!data || size < 2) {
    return;
  }

  // Simple 3-point moving average (0.25, 0.5, 0.25)
  // Forward pass with history to avoid allocation
  float prev = data[0];
  for (uint32_t i = 1; i < size; i++) {
    const float current = data[i];
    const float next = (i < size - 1) ? data[i + 1] : current;

    // Smooth current based on prev, current, next
    data[i] = (0.25F * prev) + (0.5F * current) + (0.25F * next);

    prev = current; // Save original current for next iteration's 'prev'
  }
}

void spectral_smoothing_apply_simple_temporal(float* current, float* memory,
                                              uint32_t size, float smoothing) {
  if (!current || !memory || size == 0) {
    return;
  }

  for (uint32_t i = 0; i < size; i++) {
    memory[i] = (smoothing * current[i]) + ((1.0F - smoothing) * memory[i]);
    current[i] = memory[i];
  }
}
