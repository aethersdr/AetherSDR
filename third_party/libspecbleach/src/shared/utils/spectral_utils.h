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

#ifndef SPECTRAL_UTILS_H
#define SPECTRAL_UTILS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum WindowTypes {
  HANN_WINDOW = 0,
  HAMMING_WINDOW = 1,
  BLACKMAN_WINDOW = 2,
  VORBIS_WINDOW = 3
} WindowTypes;

bool get_fft_window(float* window, uint32_t fft_size, WindowTypes window_type);
bool initialize_spectrum_with_value(float* spectrum, uint32_t spectrum_size,
                                    float value);
bool direct_matrix_to_vector_spectral_convolution(const float* matrix_spectum,
                                                  const float* spectrum,
                                                  float* out_spectrum,
                                                  uint32_t spectrum_size);
float max_spectral_value(const float* spectrum, uint32_t real_spectrum_size);
float min_spectral_value(const float* spectrum, uint32_t real_spectrum_size);

#define min_spectrum(spectrum_one, spectrum_two, spectrum_size)                \
  _Generic((spectrum_one),                                                     \
      float*: min_spectrum_float,                                              \
      double*: min_spectrum_double,                                            \
      default: min_spectrum_float)(spectrum_one, spectrum_two, spectrum_size)

#define max_spectrum(spectrum_one, spectrum_two, spectrum_size)                \
  _Generic((spectrum_one),                                                     \
      float*: max_spectrum_float,                                              \
      double*: max_spectrum_double,                                            \
      default: max_spectrum_float)(spectrum_one, spectrum_two, spectrum_size)

bool min_spectrum_float(float* spectrum_one, const float* spectrum_two,
                        uint32_t spectrum_size);
bool max_spectrum_float(float* spectrum_one, const float* spectrum_two,
                        uint32_t spectrum_size);
bool min_spectrum_double(double* spectrum_one, const double* spectrum_two,
                         uint32_t spectrum_size);
bool max_spectrum_double(double* spectrum_one, const double* spectrum_two,
                         uint32_t spectrum_size);
float fft_bin_to_freq(uint32_t bin_index, uint32_t sample_rate,
                      uint32_t fft_size);
uint32_t freq_to_fft_bin(float freq, uint32_t sample_rate, uint32_t fft_size);
float spectral_flux(const float* spectrum, const float* previous_spectrum,
                    uint32_t spectrum_size);
bool get_rolling_mean_spectrum(float* averaged_spectrum,
                               const float* current_spectrum,
                               uint32_t number_of_blocks,
                               uint32_t spectrum_size);
/**
 * @brief Computes the rolling median spectrum from a history of frames.
 *
 * @param median_spectrum Output buffer for the median spectrum.
 * @param input_spectra Array of pointers to input spectra history (size:
 * number_of_blocks).
 * @param number_of_blocks Number of historical frames to consider.
 * @param spectrum_size Size of each spectrum (in floats).
 * @return true if successful, false otherwise.
 */
bool get_rolling_median_spectrum(float* median_spectrum,
                                 const float** input_spectra,
                                 uint32_t number_of_blocks,
                                 uint32_t spectrum_size);
void smooth_spectrum(float* spectrum, uint32_t size, float smoothing_factor);
void interpolate_spectrum_gaps(float* spectrum, uint32_t size,
                               float gap_threshold);
bool get_morphed_profile(float* output_profile, const float* mean_profile,
                         const float* median_profile, const float* max_profile,
                         const float* min_profile, uint32_t size,
                         float aggressiveness);

#endif
