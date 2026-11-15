/*
libspecbleach - A spectral processing library

Copyright 2022-2026 Luciano Dato <lucianodato@gmail.com>

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

#ifndef SHARED_DENOISER_LOGIC_MASKING_VETO_H
#define SHARED_DENOISER_LOGIC_MASKING_VETO_H

#include "shared/utils/critical_bands.h"
#include "shared/utils/spectral_features.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @file masking_veto.h
 * @brief Psychoacoustic veto module for spectral denoising.
 *
 * This module implements a masking veto mechanism that moderates noise
 * reduction based on psychoacoustic masking thresholds. It ensures that
 * noise reduction is only applied where it is perceptually necessary.
 */

typedef struct MaskingVeto MaskingVeto;

/**
 * Initialize a MaskingVeto instance.
 * @param fft_size Size of the FFT
 * @param sample_rate Audio sample rate
 * @param spectrum_type Scaling type (POWER or MAGNITUDE)
 * @return Initialized MaskingVeto instance or NULL on failure
 */
MaskingVeto* masking_veto_initialize(uint32_t fft_size, uint32_t sample_rate,
                                     CriticalBandType critical_band_type,
                                     SpectrumType spectrum_type,
                                     bool use_absolute_threshold,
                                     bool use_temporal_masking);

/**
 * Free a MaskingVeto instance.
 * @param self Instance to free
 */
void masking_veto_free(MaskingVeto* self);

/**
 * Moderate noise reduction alpha values based on psychoacoustic masking.
 *
 * In bands where noise is below the masking threshold (NMR < 0dB), the
 * noise is inaudible. The veto lerps alpha toward 1.0 (no extra subtraction)
 * to preserve band energy. In bands where noise is audible (NMR > 0dB),
 * alpha is left unchanged and suppression proceeds normally.
 *
 * @param self MaskingVeto instance
 * @param smoothed_spectrum Temporally smoothed spectral magnitude/power
 * @param noise_spectrum Estimated noise profile
 * @param future_spectrum Future frame for backward masking (or NULL)
 * @param alpha In/Out oversubtraction factors to be moderated
 * @param depth Veto strength (0.0: No veto, 1.0: Full psychoacoustic
 * protection)
 */
void masking_veto_apply(MaskingVeto* self, const float* smoothed_spectrum,
                        const float* noise_spectrum,
                        const float* future_spectrum, float* alpha,
                        float depth);

#endif // SHARED_DENOISER_LOGIC_MASKING_VETO_H
