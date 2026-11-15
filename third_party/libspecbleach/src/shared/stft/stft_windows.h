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

#ifndef STFT_WINDOW_H
#define STFT_WINDOW_H

#include "../utils/spectral_utils.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct StftWindows StftWindows;

typedef enum WindowPlace { INPUT_WINDOW = 1, OUTPUT_WINDOW = 2 } WindowPlace;

StftWindows* stft_window_initialize(uint32_t stft_fft_size,
                                    uint32_t stft_frame_size,
                                    uint32_t stft_hop_size,
                                    WindowTypes input_window,
                                    WindowTypes output_window);
void stft_window_free(StftWindows* self);
bool stft_window_apply(StftWindows* self, float* frame, WindowPlace place);

#endif
