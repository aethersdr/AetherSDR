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

#ifndef SHARED_UTILS_SPECTRAL_CIRCULAR_BUFFER_H
#define SHARED_UTILS_SPECTRAL_CIRCULAR_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Opaque handle for a spectral circular buffer.
 *
 * A spectral circular buffer manages one or more buffers (layers) synchronized
 * by a common time index, allowing for temporal alignment of spectral data.
 */
typedef struct SbSpectralCircularBuffer SbSpectralCircularBuffer;

/**
 * @brief Initialize a spectral circular buffer with a fixed number of frames.
 *
 * @param num_frames Total number of frames in the buffer.
 * @return SbSpectralCircularBuffer* Pointer to the initialized buffer, or NULL
 * if allocation fails.
 */
SbSpectralCircularBuffer* spectral_circular_buffer_create(uint32_t num_frames);

/**
 * @brief Add a data layer to the circular buffer.
 *
 * Each layer represents a specific type of spectral data (e.g., FFT, magnitude,
 * noise). All layers share the same ring buffer index.
 *
 * @param self The circular buffer instance.
 * @param layer_size Size of each frame in this layer (in floats).
 * @return uint32_t The ID of the added layer, or 0xFFFFFFFF on failure.
 */
uint32_t spectral_circular_buffer_add_layer(SbSpectralCircularBuffer* self,
                                            uint32_t layer_size);

/**
 * @brief Push a frame of data into a specific layer.
 *
 * @param self The circular buffer instance.
 * @param layer_id ID of the layer to push into.
 * @param data Data frame to store.
 */
void spectral_circular_buffer_push(SbSpectralCircularBuffer* self,
                                   uint32_t layer_id, const float* data);

/**
 * @brief Retrieve a pointer to a delayed frame from a layer.
 *
 * @param self The circular buffer instance.
 * @param layer_id ID of the layer to retrieve from.
 * @param delay Frames of delay (from current write position).
 * @return float* Pointer to the delayed data, or NULL if layer_id is invalid.
 */
float* spectral_circular_buffer_retrieve(SbSpectralCircularBuffer* self,
                                         uint32_t layer_id, uint32_t delay);

/**
 * @brief Advance the write index of the circular buffer.
 *
 * Should be called once per processing loop, after pushing all required layer
 * frames.
 * @param self The circular buffer instance.
 */
void spectral_circular_buffer_advance(SbSpectralCircularBuffer* self);

/**
 * @brief Destroy the circular buffer and free all associated memory.
 *
 * @param self The circular buffer instance.
 */
void spectral_circular_buffer_free(SbSpectralCircularBuffer* self);

#endif // SHARED_UTILS_SPECTRAL_CIRCULAR_BUFFER_H
