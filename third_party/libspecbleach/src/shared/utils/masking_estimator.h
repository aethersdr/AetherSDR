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

#ifndef MASKING_ESTIMATOR_H
#define MASKING_ESTIMATOR_H

#include "shared/utils/critical_bands.h"
#include "shared/utils/spectral_features.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct MaskingEstimator MaskingEstimator;

MaskingEstimator* masking_estimation_initialize(
    uint32_t fft_size, uint32_t sample_rate,
    CriticalBandType critical_band_type, SpectrumType spectrum_type,
    bool use_absolute_threshold, bool use_temporal_masking);
void masking_estimation_free(MaskingEstimator* self);
bool compute_masking_thresholds(MaskingEstimator* self, const float* spectrum,
                                const float* future_spectrum,
                                float* masking_thresholds);

#endif
