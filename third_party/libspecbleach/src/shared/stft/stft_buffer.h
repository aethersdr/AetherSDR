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

#ifndef STFT_BUFFER_H
#define STFT_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct StftBuffer StftBuffer;
StftBuffer* stft_buffer_initialize(uint32_t stft_frame_size,
                                   uint32_t start_position,
                                   uint32_t block_step);
void stft_buffer_free(StftBuffer* self);
bool is_buffer_full(StftBuffer* self);
float stft_buffer_fill(StftBuffer* self, float input_sample);
bool stft_buffer_advance_block(StftBuffer* self,
                               const float* reconstructed_signal);
float* get_full_buffer_block(StftBuffer* self);

#endif
