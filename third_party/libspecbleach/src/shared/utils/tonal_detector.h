/*
libspecbleach - A spectral processing library
*/

#ifndef TONAL_DETECTOR_H
#define TONAL_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Identify tonal peaks in a noise profile using frequency-adaptive detection
 * with parabolic peak interpolation.
 *
 * At low frequencies, the detector uses wider background estimation windows
 * and sideband spread to account for the dense harmonic spacing. Parabolic
 * interpolation corrects for FFT scalloping loss to improve threshold accuracy.
 *
 * @param profile Noise profile (magnitude or power)
 * @param max_profile Maximum captured profile (for stationarity check)
 * @param median_profile Median captured profile (for stationarity check)
 * @param size Spectrum size (real_spectrum_size = fft_size/2 + 1)
 * @param sample_rate Audio sample rate in Hz
 * @param fft_size FFT size used for analysis
 * @param tonal_mask Output mask (0.0–1.0, higher = more tonal)
 */
void detect_tonal_components(const float* profile, const float* max_profile,
                             const float* median_profile, uint32_t size,
                             uint32_t sample_rate, uint32_t fft_size,
                             float* tonal_mask);

#endif
