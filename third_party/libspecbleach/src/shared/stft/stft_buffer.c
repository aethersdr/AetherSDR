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

#include "stft_buffer.h"
#include <stdlib.h>
#include <string.h>

struct StftBuffer {
  uint32_t read_position;
  uint32_t start_position;
  uint32_t stft_frame_size;
  uint32_t block_step;

  float* in_fifo;
  float* out_fifo;
};

StftBuffer* stft_buffer_initialize(const uint32_t stft_frame_size,
                                   const uint32_t start_position,
                                   const uint32_t block_step) {
  StftBuffer* self = (StftBuffer*)calloc(1U, sizeof(StftBuffer));
  if (!self) {
    return NULL;
  }

  self->stft_frame_size = stft_frame_size;
  self->start_position = start_position;
  self->block_step = block_step;
  self->read_position = self->start_position;
  self->in_fifo = (float*)calloc(self->stft_frame_size, sizeof(float));
  self->out_fifo = (float*)calloc(self->stft_frame_size, sizeof(float));

  if (!self->in_fifo || !self->out_fifo) {
    stft_buffer_free(self);
    return NULL;
  }

  return self;
}

void stft_buffer_free(StftBuffer* self) {
  if (!self) {
    return;
  }
  free(self->in_fifo);
  free(self->out_fifo);

  free(self);
}

bool is_buffer_full(StftBuffer* self) {
  if (self->read_position == self->stft_frame_size) {
    return true;
  }

  return false;
}

float stft_buffer_fill(StftBuffer* self, const float input_sample) {
  float sample_value = 0.F;

  self->in_fifo[self->read_position] = input_sample;
  sample_value = self->out_fifo[self->read_position - self->start_position];
  if (self->read_position < self->stft_frame_size) {
    self->read_position++; // Advance
  }

  return sample_value;
}

bool stft_buffer_advance_block(StftBuffer* self,
                               const float* reconstructed_signal) {
  if (!reconstructed_signal) {
    return false;
  }

  self->read_position = self->start_position; // Reset read

  memmove(self->in_fifo, &self->in_fifo[self->block_step],
          sizeof(float) * self->start_position);

  memcpy(self->out_fifo, reconstructed_signal,
         sizeof(float) * self->block_step);

  return true;
}

float* get_full_buffer_block(StftBuffer* self) {
  return self->in_fifo;
}
