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

#include "spectral_circular_buffer.h"
#include "shared/configurations.h"
#include <stdlib.h>
#include <string.h>

typedef struct SbCircularLayer {
  float* buffer;
  uint32_t size;
} SbCircularLayer;

struct SbSpectralCircularBuffer {
  SbCircularLayer layers[MAX_SPECTRAL_CIRCULAR_BUFFER_LAYERS];
  uint32_t num_layers;
  uint32_t num_frames;
  uint32_t write_index;
};

SbSpectralCircularBuffer* spectral_circular_buffer_create(uint32_t num_frames) {
  if (num_frames == 0) {
    return NULL;
  }

  SbSpectralCircularBuffer* self =
      (SbSpectralCircularBuffer*)calloc(1U, sizeof(SbSpectralCircularBuffer));
  if (!self) {
    return NULL;
  }

  self->num_frames = num_frames;
  self->write_index = 0;
  self->num_layers = 0;

  return self;
}

uint32_t spectral_circular_buffer_add_layer(SbSpectralCircularBuffer* self,
                                            uint32_t layer_size) {
  if (!self || self->num_layers >= MAX_SPECTRAL_CIRCULAR_BUFFER_LAYERS ||
      layer_size == 0) {
    return 0xFFFFFFFF;
  }

  uint32_t layer_id = self->num_layers++;
  self->layers[layer_id].size = layer_size;
  self->layers[layer_id].buffer =
      (float*)calloc((size_t)self->num_frames * layer_size, sizeof(float));

  if (!self->layers[layer_id].buffer) {
    self->num_layers--;
    return 0xFFFFFFFF;
  }

  return layer_id;
}

void spectral_circular_buffer_push(SbSpectralCircularBuffer* self,
                                   uint32_t layer_id, const float* data) {
  if (!self || layer_id >= self->num_layers || !data) {
    return;
  }

  memcpy(&self->layers[layer_id]
              .buffer[(size_t)self->write_index * self->layers[layer_id].size],
         data, self->layers[layer_id].size * sizeof(float));
}

float* spectral_circular_buffer_retrieve(SbSpectralCircularBuffer* self,
                                         uint32_t layer_id, uint32_t delay) {
  if (!self || layer_id >= self->num_layers) {
    return NULL;
  }

  uint32_t read_index =
      (self->write_index + self->num_frames - delay) % self->num_frames;

  return &self->layers[layer_id]
              .buffer[(size_t)read_index * self->layers[layer_id].size];
}

void spectral_circular_buffer_advance(SbSpectralCircularBuffer* self) {
  if (!self) {
    return;
  }

  self->write_index = (self->write_index + 1) % self->num_frames;
}

void spectral_circular_buffer_free(SbSpectralCircularBuffer* self) {
  if (!self) {
    return;
  }

  for (uint32_t i = 0; i < self->num_layers; i++) {
    if (self->layers[i].buffer) {
      free(self->layers[i].buffer);
    }
  }

  free(self);
}
