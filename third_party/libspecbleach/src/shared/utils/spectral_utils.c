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

#include "spectral_utils.h"
#include "../configurations.h"
#include "general_utils.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>

static float blackman(const uint32_t bin_index, const uint32_t fft_size) {
  const float p = ((float)(bin_index)) / ((float)(fft_size));
  return sanitize_denormal(0.42F - (0.5F * cosf(2.F * M_PIf * p)) +
                           (0.08F * cosf(4.F * M_PIf * p)));
}

static float hanning(const uint32_t bin_index, const uint32_t fft_size) {
  const float p = ((float)(bin_index)) / ((float)(fft_size));
  return sanitize_denormal(0.5F - (0.5F * cosf(2.F * M_PIf * p)));
}

static float hamming(const uint32_t bin_index, const uint32_t fft_size) {
  const float p = ((float)(bin_index)) / ((float)(fft_size));
  return sanitize_denormal(0.54F - (0.46F * cosf(2.F * M_PIf * p)));
}

static float vorbis(const uint32_t bin_index, const uint32_t fft_size) {
  const float p = ((float)(bin_index)) / ((float)(fft_size));
  return sanitize_denormal(sinf(M_PIf / 2.F * powf(sinf(M_PIf * p), 2.F)));
}

bool get_fft_window(float* window, const uint32_t fft_size,
                    const WindowTypes window_type) {
  if (!window || !fft_size) {
    return false;
  }

  for (uint32_t k = 0; k < fft_size; k++) {
    switch (window_type) {
      case HANN_WINDOW:
        window[k] = hanning(k, fft_size);
        break;
      case HAMMING_WINDOW:
        window[k] = hamming(k, fft_size);
        break;
      case BLACKMAN_WINDOW:
        window[k] = blackman(k, fft_size);
        break;
      case VORBIS_WINDOW:
        window[k] = vorbis(k, fft_size);
        break;
      default:
        break;
    }
  }

  return true;
}

bool initialize_spectrum_with_value(float* spectrum, uint32_t spectrum_size,
                                    const float value) {
  if (!spectrum || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t i = 0U; i < spectrum_size; i++) {
    spectrum[i] = value;
  }

  return true;
}

float max_spectral_value(const float* spectrum,
                         const uint32_t real_spectrum_size) {
  if (!spectrum || real_spectrum_size == 0U) {
    return 0.F;
  }

  float max = spectrum[0];
  for (uint32_t k = 1U; k < real_spectrum_size; k++) {
    max = fmaxf(spectrum[k], max);
  }
  return max;
}

float min_spectral_value(const float* spectrum,
                         const uint32_t real_spectrum_size) {
  if (!spectrum || real_spectrum_size == 0U) {
    return 0.F;
  }

  float min = spectrum[0];
  for (uint32_t k = 1U; k < real_spectrum_size; k++) {
    min = fminf(spectrum[k], min);
  }
  return min;
}

bool min_spectrum_float(float* spectrum_one, const float* spectrum_two,
                        const uint32_t spectrum_size) {
  if (!spectrum_one || !spectrum_two || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t k = 0; k < spectrum_size; k++) {
    spectrum_one[k] = fminf(spectrum_one[k], spectrum_two[k]);
  }

  return true;
}

bool max_spectrum_float(float* spectrum_one, const float* spectrum_two,
                        const uint32_t spectrum_size) {
  if (!spectrum_one || !spectrum_two || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t k = 0; k < spectrum_size; k++) {
    spectrum_one[k] = fmaxf(spectrum_one[k], spectrum_two[k]);
  }

  return true;
}

bool min_spectrum_double(double* spectrum_one, const double* spectrum_two,
                         const uint32_t spectrum_size) {
  if (!spectrum_one || !spectrum_two || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t k = 0; k < spectrum_size; k++) {
    spectrum_one[k] = fmin(spectrum_one[k], spectrum_two[k]);
  }

  return true;
}

bool max_spectrum_double(double* spectrum_one, const double* spectrum_two,
                         const uint32_t spectrum_size) {
  if (!spectrum_one || !spectrum_two || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t k = 0; k < spectrum_size; k++) {
    spectrum_one[k] = fmax(spectrum_one[k], spectrum_two[k]);
  }

  return true;
}

bool direct_matrix_to_vector_spectral_convolution(const float* matrix_spectum,
                                                  const float* spectrum,
                                                  float* out_spectrum,
                                                  uint32_t spectrum_size) {
  if (!matrix_spectum || !spectrum || !out_spectrum || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t i = 0U; i < spectrum_size; i++) {
    out_spectrum[i] = 0.F;
    for (uint32_t j = 0U; j < spectrum_size; j++) {
      out_spectrum[i] +=
          (matrix_spectum[(i * spectrum_size) + j] * spectrum[j]);
    }
  }

  return true;
}

float fft_bin_to_freq(const uint32_t bin_index, const uint32_t sample_rate,
                      const uint32_t fft_size) {
  return (float)bin_index * ((float)sample_rate / (float)fft_size);
}

uint32_t freq_to_fft_bin(const float freq, const uint32_t sample_rate,
                         const uint32_t fft_size) {
  return (uint32_t)((freq / ((float)sample_rate / (float)fft_size)) + 0.5f);
}

float spectral_flux(const float* spectrum, const float* previous_spectrum,
                    const uint32_t spectrum_size) {
  if (!spectrum || !previous_spectrum || spectrum_size == 0U) {
    return 0.F;
  }

  float spectral_flux = 0.F;

  for (uint32_t i = 0U; i < spectrum_size; i++) {
    const float temp = sqrtf(spectrum[i]) - sqrtf(previous_spectrum[i]);
    spectral_flux += (temp + fabsf(temp)) / 2.F;
  }
  return spectral_flux;
}

bool get_rolling_mean_spectrum(float* averaged_spectrum,
                               const float* current_spectrum,
                               const uint32_t number_of_blocks,
                               const uint32_t spectrum_size) {
  if (!averaged_spectrum || !current_spectrum || spectrum_size == 0U) {
    return false;
  }

  for (uint32_t k = 0U; k < spectrum_size; k++) {
    if (number_of_blocks <= 1U) {
      averaged_spectrum[k] = current_spectrum[k];
    } else {
      averaged_spectrum[k] += (current_spectrum[k] - averaged_spectrum[k]) /
                              (float)number_of_blocks;
    }
  }

  return true;
}

static int min_max_comparator(const void* a, const void* b) {
  float x = *(const float*)a;
  float y = *(const float*)b;

  return x >= y ? 1 : -1;
}

static float find_median(const float* array, uint32_t array_size) {
  float median = 0.F;

  if (array_size % 2 == 0) {
    // if number of elements are even
    median = (array[(array_size - 1U) / 2U] + array[array_size / 2U]) / 2.F;
  } else {
    // if number of elements are odd
    median = array[array_size / 2U];
  }

  return median;
}

bool get_rolling_median_spectrum(float* median_spectrum,
                                 const float** input_spectra,
                                 const uint32_t number_of_blocks,
                                 const uint32_t spectrum_size) {
  if (!median_spectrum || !input_spectra || spectrum_size == 0U) {
    return false;
  }

  // Optimize for small block counts to avoid VLA if possible, but keep simple
  // for now Note: VLA usage is generally discouraged in strict C
  // standards/security contexts, but this matches existing pattern.
  float tmp_buffer[number_of_blocks];

  for (uint32_t i = 0U; i < spectrum_size; i++) {
    for (uint32_t j = 0U; j < number_of_blocks; j++) {
      if (input_spectra[j]) {
        tmp_buffer[j] = input_spectra[j][i];
      } else {
        tmp_buffer[j] = 0.0f; // Safety fallback
      }
    }

    // Sorting array
    qsort(tmp_buffer, number_of_blocks, sizeof(float), min_max_comparator);

    median_spectrum[i] = find_median(tmp_buffer, number_of_blocks);
  }

  return true;
}

void smooth_spectrum(float* spectrum, uint32_t size, float smoothing_factor) {
  if (!spectrum || size < 2 || smoothing_factor <= 0.0F) {
    return;
  }

  // Boundary check: Handle edge bins with 2-point moving average
  // to ensure smoothing reaches the very last bin (Nyquist)
  float first = spectrum[0];
  spectrum[0] = ((spectrum[0] + spectrum[1]) / 2.0F * smoothing_factor) +
                (spectrum[0] * (1.0F - smoothing_factor));

  float prev = first;
  for (uint32_t i = 1; i < size - 1; i++) {
    float current = spectrum[i];
    spectrum[i] =
        (((prev + current + spectrum[i + 1]) / 3.0F) * smoothing_factor) +
        (current * (1.0F - smoothing_factor));
    prev = current;
  }

  // Handle last bin (Nyquist)
  spectrum[size - 1] =
      (((prev + spectrum[size - 1]) / 2.0F) * smoothing_factor) +
      (spectrum[size - 1] * (1.0F - smoothing_factor));
}

void interpolate_spectrum_gaps(float* spectrum, uint32_t size,
                               float gap_threshold) {
  if (!spectrum || size < 3) {
    return;
  }

  for (uint32_t i = 1; i < size - 1; i++) {
    if (spectrum[i] < gap_threshold) {
      // Find next non-gap bin
      uint32_t j = i + 1;
      while (j < size && spectrum[j] < gap_threshold) {
        j++;
      }

      if (j < size) {
        // Interpolate between i-1 and j
        float start_val = spectrum[i - 1];
        float end_val = spectrum[j];
        float step = (end_val - start_val) / (float)(j - (i - 1));

        for (uint32_t k = i; k < j; k++) {
          spectrum[k] = start_val + (step * (float)(k - (i - 1)));
        }
        i = j;
      }
    }
  }
}

bool get_morphed_profile(float* output_profile, const float* mean_profile,
                         const float* median_profile, const float* max_profile,
                         const float* min_profile, uint32_t size,
                         float aggressiveness) {
  if (!output_profile || !mean_profile || !median_profile || !max_profile ||
      !min_profile || size == 0) {
    return false;
  }

  for (uint32_t i = 0; i < size; i++) {
    if (aggressiveness < 0.0F) {
      // Morph from Mean (0) to Median (-1)
      float t = -aggressiveness;
      output_profile[i] =
          (mean_profile[i] * (1.0F - t)) + (median_profile[i] * t);
    } else {
      // Morph from Mean (0) to Max (1)
      float t = aggressiveness;
      output_profile[i] = (mean_profile[i] * (1.0F - t)) + (max_profile[i] * t);
    }

    // Always ensure the profile is at least the Minimum
    output_profile[i] = fmaxf(output_profile[i], min_profile[i]);
  }

  return true;
}
