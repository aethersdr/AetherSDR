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

#include "stft_windows.h"
#include <stdlib.h>

#include <float.h> // For FLT_EPSILON

static float get_windows_scale_factor(StftWindows* self);

struct StftWindows {
  float* input_window;
  float* output_window;
  float* analysis_input_window;
  float* analysis_output_window;
  float scale_factor;

  uint32_t stft_fft_size;
  uint32_t stft_frame_size;
  uint32_t stft_hop_size;
};

StftWindows* stft_window_initialize(uint32_t stft_fft_size,
                                    uint32_t stft_frame_size,
                                    uint32_t stft_hop_size,
                                    WindowTypes input_window,
                                    WindowTypes output_window) {
  StftWindows* self = (StftWindows*)calloc(1U, sizeof(StftWindows));
  if (!self) {
    return NULL;
  }

  self->stft_fft_size = stft_fft_size;
  self->stft_frame_size = stft_frame_size;
  self->stft_hop_size = stft_hop_size;

  self->input_window = (float*)calloc(self->stft_fft_size, sizeof(float));
  self->output_window = (float*)calloc(self->stft_fft_size, sizeof(float));
  self->analysis_input_window =
      (float*)calloc(self->stft_frame_size, sizeof(float));
  self->analysis_output_window =
      (float*)calloc(self->stft_frame_size, sizeof(float));

  if (!self->input_window || !self->output_window ||
      !self->analysis_input_window || !self->analysis_output_window) {
    stft_window_free(self);
    return NULL;
  }

  (void)get_fft_window(self->analysis_input_window, self->stft_frame_size,
                       input_window);
  (void)get_fft_window(self->analysis_output_window, self->stft_frame_size,
                       output_window);

  // Center and zero-pad windows in fft_size buffer
  uint32_t copy_pos = (self->stft_fft_size / 2U) - (self->stft_frame_size / 2U);
  for (uint32_t i = 0; i < self->stft_frame_size; i++) {
    self->input_window[copy_pos + i] = self->analysis_input_window[i];
    self->output_window[copy_pos + i] = self->analysis_output_window[i];
  }

  self->scale_factor = get_windows_scale_factor(self);

  return self;
}

void stft_window_free(StftWindows* self) {
  if (!self) {
    return;
  }
  if (self->input_window) {
    free(self->input_window);
  }
  if (self->output_window) {
    free(self->output_window);
  }
  if (self->analysis_input_window) {
    free(self->analysis_input_window);
  }
  if (self->analysis_output_window) {
    free(self->analysis_output_window);
  }
  free(self);
}

static float get_windows_scale_factor(StftWindows* self) {
  if (!self->output_window || self->stft_hop_size == 0) {
    return 1.0f;
  }

  float sum = 0.0f;
  for (uint32_t k = 0U; k < self->stft_frame_size; k++) {
    sum += (self->analysis_output_window[k] * self->analysis_output_window[k]);
  }

  if (sum < FLT_EPSILON) {
    return 1.0f;
  }

  return sum * ((float)self->stft_fft_size / (float)self->stft_hop_size);
}

bool stft_window_apply(StftWindows* self, float* frame, WindowPlace place) {
  if (!self || !frame) {
    return false;
  }

  float* window =
      (place == INPUT_WINDOW) ? self->input_window : self->output_window;

  for (uint32_t k = 0U; k < self->stft_fft_size; k++) {
    frame[k] *= window[k];
  }

  if (place == OUTPUT_WINDOW) {
    for (uint32_t k = 0U; k < self->stft_fft_size; k++) {
      frame[k] /= self->scale_factor;
    }
  }

  return true;
}
