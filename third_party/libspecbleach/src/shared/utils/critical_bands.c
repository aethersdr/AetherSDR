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

#include "critical_bands.h"
#include "spectral_utils.h"
#include <stdlib.h>
#include <string.h>

static const float bark_bands[25] = {
    100.F,  200.F,  300.F,  400.F,  510.F,   630.F,   770.F,  920.F,  1080.F,
    1270.F, 1480.F, 1720.F, 2000.F, 2320.F,  2700.F,  3150.F, 3700.F, 4400.F,
    5300.F, 6400.F, 7700.F, 9500.F, 12000.F, 15500.F, 19000.F};
static const float opus_bands[20] = {200.F,  400.F,  600.F,  800.F,   1000.F,
                                     1200.F, 1400.F, 1600.F, 2000.F,  2400.F,
                                     2800.F, 3200.F, 4000.F, 4800.F,  5600.F,
                                     6800.F, 8000.F, 9600.F, 12000.F, 15600.F};
static const float mel_bands[33] = {
    250.F,  500.F,  750.F,  1000.F, 1250.F, 1500.F, 1750.F, 2000.F,
    2250.F, 2500.F, 2750.F, 3000.F, 3250.F, 3500.F, 3750.F, 4000.F,
    4250.F, 4500.F, 4750.F, 5000.F, 5250.F, 5500.F, 5750.F, 6000.F,
    6250.F, 6500.F, 6750.F, 7000.F, 7250.F, 7500.F, 7750.F, 8000.F};
static const float octave_bands[10] = {31.5F,  63.F,   125.F,  250.F,  500.F,
                                       1000.F, 2000.F, 4000.F, 8000.F, 16000.F};

void set_number_of_bands(CriticalBands* self);
static void compute_mapping_spectrum(CriticalBands* self);
static void compute_band_indexes(CriticalBands* self);
static uint32_t get_last_valid_band_for_samplerate(CriticalBands* self,
                                                   uint32_t number_of_bands);

struct CriticalBands {
  uint32_t* band_delimiter_bins;
  uint32_t* number_bins_per_band;
  float* current_critical_bands;

  uint32_t fft_size;
  uint32_t real_spectrum_size;
  uint32_t sample_rate;
  uint32_t number_bands;
  CriticalBandType type;
  CriticalBandIndexes band_indexes;
};

CriticalBands* critical_bands_initialize(const uint32_t sample_rate,
                                         const uint32_t fft_size,
                                         const CriticalBandType type) {

  CriticalBands* self = (CriticalBands*)calloc(1U, sizeof(CriticalBands));

  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (fft_size / 2U) + 1U;
  self->sample_rate = sample_rate;
  self->type = type;

  compute_mapping_spectrum(self);

  self->band_delimiter_bins =
      (uint32_t*)calloc(self->number_bands, sizeof(uint32_t));
  self->number_bins_per_band =
      (uint32_t*)calloc(self->number_bands, sizeof(uint32_t));

  if (!self->band_delimiter_bins || !self->number_bins_per_band) {
    critical_bands_free(self);
    return NULL;
  }

  compute_band_indexes(self);

  return self;
}

void critical_bands_free(CriticalBands* self) {
  if (!self) {
    return;
  }
  free(self->band_delimiter_bins);
  free(self->number_bins_per_band);

  free(self);
}

static void compute_band_indexes(CriticalBands* self) {
  for (uint32_t k = 0U; k < self->number_bands; k++) {

    const uint32_t bin_index =
        freq_to_fft_bin(self->current_critical_bands[k], self->sample_rate,
                        self->real_spectrum_size);

    if (k == 0) {
      self->number_bins_per_band[k] = bin_index; // Don't include DC bin
      self->band_delimiter_bins[k] = bin_index;
    } else if (k == self->number_bands - 1U) {
      self->band_delimiter_bins[k] = self->real_spectrum_size;
      self->number_bins_per_band[k] =
          self->band_delimiter_bins[k] - self->band_delimiter_bins[k - 1];
    } else {
      self->number_bins_per_band[k] =
          bin_index - self->band_delimiter_bins[k - 1];
      self->band_delimiter_bins[k] = bin_index;
    }
  }
}

static void compute_mapping_spectrum(CriticalBands* self) {
  switch (self->type) {
    case BARK_SCALE: {
      self->current_critical_bands = (float*)bark_bands;
      uint32_t number_of_bark_bands = sizeof(bark_bands) / sizeof(float);
      self->number_bands =
          get_last_valid_band_for_samplerate(self, number_of_bark_bands);
      break;
    }
    case MEL_SCALE: {
      self->current_critical_bands = (float*)mel_bands;
      uint32_t number_of_mel_bands = sizeof(mel_bands) / sizeof(float);
      self->number_bands =
          get_last_valid_band_for_samplerate(self, number_of_mel_bands);
      break;
    }
    case OPUS_SCALE: {
      self->current_critical_bands = (float*)opus_bands;
      uint32_t number_of_opus_bands = sizeof(opus_bands) / sizeof(float);
      self->number_bands =
          get_last_valid_band_for_samplerate(self, number_of_opus_bands);
      break;
    }
    case OCTAVE_SCALE: {
      self->current_critical_bands = (float*)octave_bands;
      uint32_t number_of_octave_bands = sizeof(octave_bands) / sizeof(float);
      self->number_bands =
          get_last_valid_band_for_samplerate(self, number_of_octave_bands);
      break;
    }
    default:
      break;
  }
}

static uint32_t get_last_valid_band_for_samplerate(CriticalBands* self,
                                                   uint32_t number_of_bands) {
  float nyquist_frequency = (float)self->sample_rate / 2.F;
  uint32_t last_valid_band = 0U;

  for (uint32_t i = 0; i < number_of_bands; i++) {
    if (self->current_critical_bands[i] < nyquist_frequency) {
      last_valid_band = i;
    }
  }

  return last_valid_band + 1;
}
bool compute_critical_bands_spectrum(CriticalBands* self, const float* spectrum,
                                     float* critical_bands) {
  if (!self || !spectrum || !critical_bands) {
    return false;
  }

  memset(critical_bands, 0, self->number_bands * sizeof(float));

  for (uint32_t j = 0U; j < self->number_bands; j++) {
    self->band_indexes = get_band_indexes(self, j);

    for (uint32_t k = self->band_indexes.start_position;
         k < self->band_indexes.end_position; k++) {
      critical_bands[j] += spectrum[k];
    }
  }

  return true;
}

CriticalBandIndexes get_band_indexes(CriticalBands* self,
                                     const uint32_t band_number) {
  return (CriticalBandIndexes){
      .start_position = self->band_delimiter_bins[band_number] -
                        self->number_bins_per_band[band_number],
      .end_position = self->band_delimiter_bins[band_number],
  };
}

uint32_t get_number_of_critical_bands(CriticalBands* self) {
  return self->number_bands;
}
