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

#ifndef GAIN_CALCULATOR_H
#define GAIN_CALCULATOR_H

#include <stdbool.h>
#include <stdint.h>

typedef enum GainCalculationType {
  WIENER = 0,
  GATES = 1,
  GENERALIZED_SPECTRALSUBTRACTION = 2,
} GainCalculationType;

void calculate_gains(uint32_t real_spectrum_size, uint32_t fft_size,
                     const float* spectrum, const float* noise_spectrum,
                     float* gain_spectrum, const float* alpha,
                     const float* beta, GainCalculationType type);

#endif // GAIN_CALCULATOR_H
