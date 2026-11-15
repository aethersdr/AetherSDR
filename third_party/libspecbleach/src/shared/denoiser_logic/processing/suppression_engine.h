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

#ifndef SUPPRESSION_ENGINE_H
#define SUPPRESSION_ENGINE_H

#include <stdint.h>

/**
 * @file suppression_engine.h
 * @brief SNR-dependent and psychoacoustic oversubtraction (Alpha) calculation
 * engine.
 *
 * This module implements various oversubtraction factor calculation strategies,
 * including Berouti-style per-bin SNR scaling, global SNR scaling,
 * critical band SNR scaling, and NMR-based psychoacoustic scaling.
 */

#include "shared/utils/critical_bands.h"
#include "shared/utils/spectral_features.h"

typedef enum SuppressionType {
  SUPPRESSION_BEROUTI_PER_BIN = 0,    // Per-bin SNR-dependent oversubtraction
  SUPPRESSION_GLOBAL_SNR = 1,         // Global SNR-based scaling
  SUPPRESSION_CRITICAL_BANDS_SNR = 2, // Critical band SNR-based scaling
  SUPPRESSION_MASKING_THRESHOLDS =
      3, // NMR-based scaling using psychoacoustic model
  SUPPRESSION_NONE = 4
} SuppressionType;

typedef struct SuppressionParameters {
  SuppressionType type;
  float strength;         // Maps to oversubtraction depth (0.0 to 1.0)
  float undersubtraction; // Maps to beta factor (0.0 to 1.0)
} SuppressionParameters;

typedef struct SuppressionEngine SuppressionEngine;

/**
 * Initialize a SuppressionEngine instance.
 * @param real_spectrum_size Size of the real spectrum ((fft_size/2)+1)
 * @param sample_rate Sample rate of the audio
 * @param critical_band_type Type of critical bands to use
 * @param spectrum_type Type of spectral representation
 * @return Initialized instance or NULL on failure
 */
SuppressionEngine* suppression_engine_initialize(
    uint32_t real_spectrum_size, uint32_t sample_rate,
    CriticalBandType critical_band_type, SpectrumType spectrum_type,
    bool use_absolute_threshold, bool use_temporal_masking);

/**
 * Free a SuppressionEngine instance.
 * @param self Instance to free
 */
void suppression_engine_free(SuppressionEngine* self);

/**
 * Calculate per-bin oversubtraction (alpha) and undersubtraction (beta)
 * factors based on the selected strategy.
 *
 * @param self SuppressionEngine instance
 * @param reference_spectrum Current spectral magnitude/power
 * @param noise_spectrum Estimated noise profile
 * @param parameters Suppression configuration
 * @param alpha [Out] Calculated oversubtraction factors
 * @param beta [Out] Calculated undersubtraction factors
 */
void suppression_engine_calculate(SuppressionEngine* self,
                                  const float* reference_spectrum,
                                  const float* noise_spectrum,
                                  SuppressionParameters parameters,
                                  float* alpha, float* beta);

#endif
