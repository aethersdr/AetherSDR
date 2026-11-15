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

#ifndef NLM_FILTER_H
#define NLM_FILTER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct NlmFilter NlmFilter;

/**
 * Configuration for Non-Local Means filter with Lukin optimizations.
 * Based on "Suppression of Musical Noise Artifacts" by Lukin & Todd.
 */
typedef struct NlmFilterConfig {
  uint32_t spectrum_size;          /**< Number of frequency bins (real spectrum
                                        size) */
  uint32_t time_buffer_size;       /**< Number of frames to buffer (e.g., 21
                                        for -16/+4 search range) */
  uint32_t patch_size;             /**< Patch size for matching (default: 8)
                                    */
  uint32_t paste_block_size;       /**< Output block size for pasted block
                                        optimization (default: 4) */
  uint32_t search_range_freq;      /**< Search range in frequency bins
                                        (default: 8) */
  uint32_t search_range_time_past; /**< Search range in time - past frames
                                        (default: 16) */
  uint32_t search_range_time_future; /**< Search range in time - future frames
                                          (default: 4) */
  float h_parameter;                 /**< Smoothing parameter controlling weight
                                          decay */
  float distance_threshold;          /**< Skip patches with distance >
                                          threshold (default: 4*h²) */
} NlmFilterConfig;

/**
 * Initialize an NLM filter with the given configuration.
 * @param config Filter configuration parameters
 * @return Pointer to initialized filter, or NULL on failure
 */
NlmFilter* nlm_filter_initialize(NlmFilterConfig config);

/**
 * Free resources associated with an NLM filter.
 * @param filter Pointer to filter to free
 */
void nlm_filter_free(NlmFilter* filter);

/**
 * Update the smoothing parameter at runtime.
 * @param filter Pointer to filter
 * @param h New smoothing parameter value
 */
void nlm_filter_set_h_parameter(NlmFilter* filter, float h);

/**
 * Push a new SNR frame into the ring buffer.
 * SNR frame should be computed as: snr[k] = magnitude[k] / noise_profile[k]
 * @param filter Pointer to filter
 * @param snr_frame Array of SNR values (size: spectrum_size)
 */
void nlm_filter_push_frame(NlmFilter* filter, const float* snr_frame);

/**
 * Process the current target frame and compute smoothed SNR output.
 * Uses pasted block optimization for efficiency.
 * @param filter Pointer to filter
 * @param smoothed_snr Output array for smoothed SNR (size: spectrum_size)
 * @return true if processing succeeded, false if buffer not yet full
 */
bool nlm_filter_process(NlmFilter* filter, float* smoothed_snr);

/**
 * Check if the filter buffer is ready for processing.
 * @param filter Pointer to filter
 * @return true if buffer is full and ready
 */
bool nlm_filter_is_ready(NlmFilter* filter);

/**
 * Reset the filter state, clearing all buffered frames.
 * @param filter Pointer to filter
 */
void nlm_filter_reset(NlmFilter* filter);

/**
 * Get the additional latency introduced by NLM look-ahead.
 * @param filter Pointer to filter
 * @return Number of frames of look-ahead latency
 */
uint32_t nlm_filter_get_latency_frames(NlmFilter* filter);

/**
 * Calculate the SNR frame from reference and noise spectra.
 * snr[k] = reference[k] / noise[k] (with epsilon safety)
 * @param filter Pointer to filter
 * @param reference_spectrum Input signal spectrum
 * @param noise_spectrum Noise profile spectrum
 * @param snr_frame Output SNR frame
 */
void nlm_filter_calculate_snr(NlmFilter* filter,
                              const float* reference_spectrum,
                              const float* noise_spectrum, float* snr_frame);

/**
 * Reconstruct the magnitude spectrum from smoothed SNR and noise.
 * magnitude[k] = smoothed_snr[k] * noise[k] (with epsilon safety)
 * @param filter Pointer to filter
 * @param smoothed_snr Input smoothed SNR
 * @param noise_spectrum Noise profile spectrum
 * @param magnitude_spectrum Output reconstructed magnitude
 */
void nlm_filter_reconstruct_magnitude(NlmFilter* filter,
                                      const float* smoothed_snr,
                                      const float* noise_spectrum,
                                      float* magnitude_spectrum);

#endif /* NLM_FILTER_H */
