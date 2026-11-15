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

#ifndef STFT_PROCESSOR_H
#define STFT_PROCESSOR_H

#include "../spectral_processor.h"
#include "../utils/spectral_utils.h"
#include "fft_transform.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct StftProcessor StftProcessor;

StftProcessor* stft_processor_initialize(
    uint32_t sample_rate, float stft_frame_size, uint32_t overlap_factor,
    ZeroPaddingType padding_type, uint32_t zeropadding_amount,
    WindowTypes input_window, WindowTypes output_window);
void stft_processor_free(StftProcessor* self);
uint32_t get_stft_latency(StftProcessor* self);
uint32_t get_stft_fft_size(StftProcessor* self);
uint32_t get_stft_real_spectrum_size(StftProcessor* self);

// Receives an input and output buffer with a a number_of_samples and does the
// STFT transform applying any spectral_processing. It works similar to qsort,
// because it receives a function pointer of any spectral processing that needs
// to be applied in between the analysis and the synthesis
bool stft_processor_run(StftProcessor* self, uint32_t number_of_samples,
                        const float* input, float* output,
                        spectral_processing spectral_processing,
                        SpectralProcessorHandle spectral_processor);

#endif
