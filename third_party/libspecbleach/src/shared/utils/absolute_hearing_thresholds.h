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

#ifndef ABSOLUTE_HEARING_THRESHOLDS_H
#define ABSOLUTE_HEARING_THRESHOLDS_H

#include "../utils/spectral_features.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct AbsoluteHearingThresholds AbsoluteHearingThresholds;

AbsoluteHearingThresholds* absolute_hearing_thresholds_initialize(
    uint32_t sample_rate, uint32_t fft_size, SpectrumType spectrum_type);
void absolute_hearing_thresholds_free(AbsoluteHearingThresholds* self);
bool apply_thresholds_as_floor(AbsoluteHearingThresholds* self,
                               float* spectrum);

#endif
