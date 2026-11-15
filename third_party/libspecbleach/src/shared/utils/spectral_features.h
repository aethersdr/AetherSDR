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

#ifndef SPECTRAL_FEATURES_H
#define SPECTRAL_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SpectralFeatures SpectralFeatures;

typedef enum SpectrumType {
  POWER_SPECTRUM = 0,
  MAGNITUDE_SPECTRUM = 1,
  PHASE_SPECTRUM = 2,
} SpectrumType;

SpectralFeatures* spectral_features_initialize(uint32_t real_spectrum_size);
void spectral_features_free(SpectralFeatures* self);
float* get_spectral_feature(SpectralFeatures* self, const float* fft_spectrum,
                            uint32_t fft_spectrum_size, SpectrumType type);

#endif
