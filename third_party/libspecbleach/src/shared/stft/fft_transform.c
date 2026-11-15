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

#include "fft_transform.h"
#include "../utils/general_utils.h"

#include <fftw3.h>
#include <stdlib.h>
#include <string.h>

static uint32_t calculate_fft_size(FftTransform* self);
static bool allocate_fftw(FftTransform* self);

struct FftTransform {
  fftwf_plan forward;
  fftwf_plan backward;

  uint32_t fft_size;
  uint32_t frame_size;
  uint32_t zeropadding_amount;
  uint32_t copy_position;
  ZeroPaddingType padding_type;
  uint32_t padding_amount;
  float* input_fft_buffer;
  float* output_fft_buffer;
};

FftTransform* fft_transform_initialize(const uint32_t frame_size,
                                       const ZeroPaddingType padding_type,
                                       const uint32_t zeropadding_amount) {
  FftTransform* self = (FftTransform*)calloc(1U, sizeof(FftTransform));
  if (!self) {
    return NULL;
  }

  self->padding_type = padding_type;
  self->zeropadding_amount = zeropadding_amount;
  self->frame_size = frame_size;

  self->fft_size = calculate_fft_size(self);

  self->copy_position = (self->fft_size / 2U) - (self->frame_size / 2U);

  if (!allocate_fftw(self)) {
    fft_transform_free(self);
    return NULL;
  }

  return self;
}

FftTransform* fft_transform_initialize_bins(const uint32_t fft_size) {
  FftTransform* self = (FftTransform*)calloc(1U, sizeof(FftTransform));
  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->frame_size = self->fft_size;

  if (!allocate_fftw(self)) {
    fft_transform_free(self);
    return NULL;
  }

  return self;
}

static bool allocate_fftw(FftTransform* self) {
  self->input_fft_buffer = (float*)fftwf_malloc(self->fft_size * sizeof(float));
  self->output_fft_buffer =
      (float*)fftwf_malloc(self->fft_size * sizeof(float));

  if (!self->input_fft_buffer || !self->output_fft_buffer) {
    return false;
  }

  memset(self->input_fft_buffer, 0, self->fft_size * sizeof(float));
  memset(self->output_fft_buffer, 0, self->fft_size * sizeof(float));

  self->forward =
      fftwf_plan_r2r_1d((int)self->fft_size, self->input_fft_buffer,
                        self->output_fft_buffer, FFTW_R2HC, FFTW_ESTIMATE);
  self->backward =
      fftwf_plan_r2r_1d((int)self->fft_size, self->output_fft_buffer,
                        self->input_fft_buffer, FFTW_HC2R, FFTW_ESTIMATE);

  return self->forward && self->backward;
}

static uint32_t calculate_fft_size(FftTransform* self) {
  switch (self->padding_type) {
    case NO_PADDING: {
      self->padding_amount = 0;
      return get_next_divisible_two((int)self->frame_size);
    }
    case NEXT_POWER_OF_TWO: {
      uint32_t next_power_of_two = get_next_power_two((int)self->frame_size);
      self->padding_amount = next_power_of_two - self->frame_size;
      return next_power_of_two;
    }
    case FIXED_AMOUNT: {
      self->padding_amount = self->zeropadding_amount;
      return get_next_divisible_two(
          (int)(self->frame_size + self->padding_amount));
    }
    default:
      return get_next_divisible_two((int)self->frame_size);
  }
}

void fft_transform_free(FftTransform* self) {
  if (!self) {
    return;
  }

  if (self->input_fft_buffer) {
    fftwf_free(self->input_fft_buffer);
  }
  if (self->output_fft_buffer) {
    fftwf_free(self->output_fft_buffer);
  }

  // FFTW plans can be NULL if initialization failed
  if (self->forward) {
    fftwf_destroy_plan(self->forward);
  }
  if (self->backward) {
    fftwf_destroy_plan(self->backward);
  }

  free(self);
}

uint32_t get_fft_size(FftTransform* self) {
  if (!self) {
    return 0;
  }
  return self->fft_size;
}
uint32_t get_fft_real_spectrum_size(FftTransform* self) {
  if (!self) {
    return 0;
  }
  return (self->fft_size / 2U) + 1U;
}

bool fft_load_input_samples(FftTransform* self, const float* input) {
  if (!self || !input) {
    return false;
  }

  // Ensure buffer bounds are safe
  if (self->frame_size + self->copy_position > self->fft_size) {
    return false;
  }

  // Copy centered values only
  for (uint32_t i = self->copy_position;
       i < (self->frame_size + self->copy_position); i++) {
    self->input_fft_buffer[i] = input[i - self->copy_position];
  }

  return true;
}

bool fft_get_output_samples(FftTransform* self, float* output) {
  if (!self || !output) {
    return false;
  }

  // Ensure buffer bounds are safe
  if (self->frame_size + self->copy_position > self->fft_size) {
    return false;
  }

  // Copy centered values only
  for (uint32_t i = self->copy_position;
       i < (self->frame_size + self->copy_position); i++) {
    output[i - self->copy_position] = self->input_fft_buffer[i];
  }

  return true;
}

bool compute_forward_fft(FftTransform* self) {
  if (!self) {
    return false;
  }

  fftwf_execute(self->forward);

  return true;
}

bool compute_backward_fft(FftTransform* self) {
  if (!self) {
    return false;
  }

  fftwf_execute(self->backward);

  return true;
}

float* get_fft_input_buffer(FftTransform* self) {
  return self->input_fft_buffer;
}

float* get_fft_output_buffer(FftTransform* self) {
  return self->output_fft_buffer;
}
