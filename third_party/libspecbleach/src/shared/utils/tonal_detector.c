/*
libspecbleach - A spectral processing library
*/

#include "shared/utils/tonal_detector.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "shared/configurations.h"

static int float_comparator(const void* a, const void* b) {
  float x = *(const float*)a;
  float y = *(const float*)b;
  return (x > y) - (x < y);
}

// Smooth linear interpolation between two breakpoints
static float lerp_by_freq(float freq_hz, float val_low, float val_high) {
  if (freq_hz <= LOW_FREQ_HZ) {
    return val_low;
  }
  if (freq_hz >= MID_FREQ_HZ) {
    return val_high;
  }
  float t = (freq_hz - LOW_FREQ_HZ) / (MID_FREQ_HZ - LOW_FREQ_HZ);
  return val_low + (t * (val_high - val_low));
}

void detect_tonal_components(const float* profile, const float* max_profile,
                             const float* median_profile, uint32_t size,
                             uint32_t sample_rate, uint32_t fft_size,
                             float* tonal_mask) {
  if (!profile || !tonal_mask || size < 5 || !max_profile || !median_profile ||
      sample_rate == 0 || fft_size == 0) {
    return;
  }

  memset(tonal_mask, 0, size * sizeof(float));

  // Hz per FFT bin
  const float bin_hz = (float)sample_rate / (float)fft_size;

  // We use the median profile for detection as it is much more stable than
  // Mean/Instantaneous
  const float* detection_profile = median_profile;

  for (uint32_t k = 1; k < size - 2; k++) {
    // 1. Robust Local Maximum Check (with valley awareness)
    // Check if k is a peak relative to immediate neighbors
    bool is_local_peak = (detection_profile[k] >= detection_profile[k - 1]) &&
                         (detection_profile[k] >= detection_profile[k + 1]);

    if (is_local_peak) {
      // Check isolation from k-2: either strictly greater, OR k-1 is a valley
      // For k < 2, skip left isolation (only DC below — trusted)
      bool isolated_left = true;
      if (k >= 2) {
        isolated_left = (detection_profile[k] > detection_profile[k - 2]) ||
                        (detection_profile[k - 1] < detection_profile[k - 2] &&
                         detection_profile[k - 1] < detection_profile[k]);
      }

      // Check isolation from k+2: either strictly greater, OR k+1 is a valley
      bool isolated_right =
          (detection_profile[k] > detection_profile[k + 2]) ||
          (detection_profile[k + 1] < detection_profile[k + 2] &&
           detection_profile[k + 1] < detection_profile[k]);

      if (isolated_left && isolated_right) {

        // Compute frequency for this bin
        float freq_hz = (float)k * bin_hz;

        // 2. Parabolic interpolation for sub-bin peak accuracy
        // Fit a parabola through log-magnitudes at (k-1, k, k+1)
        float log_prev = logf(detection_profile[k - 1] + 1e-20f);
        float log_peak = logf(detection_profile[k] + 1e-20f);
        float log_next = logf(detection_profile[k + 1] + 1e-20f);

        float denom_parab = log_prev - (2.0f * log_peak) + log_next;
        float true_amplitude = detection_profile[k];

        // Only interpolate if the parabola is concave (valid peak)
        if (denom_parab < -1e-10f) {
          float delta = 0.5f * (log_prev - log_next) / denom_parab;
          // Clamp delta to [-0.5, 0.5]
          delta = fmaxf(-0.5f, fminf(0.5f, delta));
          true_amplitude =
              expf(log_peak - (0.25f * (log_prev - log_next) * delta));
        }

        // 3. Frequency-adaptive background estimation
        int bg_radius = (int)roundf(
            lerp_by_freq(freq_hz, (float)BG_RADIUS_LOW, (float)BG_RADIUS_HIGH));

        float neighbors[MAX_NEIGHBORS];
        int n_count = 0;
        for (int i = -bg_radius; i <= bg_radius; i++) {
          int idx = (int)k + i;
          if (i != 0 && idx >= 0 && idx < (int)size &&
              n_count < MAX_NEIGHBORS) {
            neighbors[n_count++] = detection_profile[idx];
          }
        }

        if (n_count > 0) {
          // Sort to find local background median
          qsort(neighbors, n_count, sizeof(float), float_comparator);
          float avg_background = neighbors[n_count / 2];

          // 4. Stationarity Index (Spread)
          float spread = (max_profile[k] + 1e-9f) / (median_profile[k] + 1e-9f);

          // Stationarity Weight [0.0 - 1.0]
          float stationarity_weight = fmaxf(
              0.0f, 1.0f - ((spread - 1.0f) / (STATIONARITY_THRESHOLD - 1.0f)));

          // 5. Frequency-adaptive peak threshold
          float threshold_factor = lerp_by_freq(freq_hz, THRESHOLD_FACTOR_LOW,
                                                THRESHOLD_FACTOR_HIGH);

          float threshold = PEAK_THRESHOLD * threshold_factor *
                            (1.5f - (stationarity_weight * 0.5f));

          // Use parabolic-corrected amplitude for threshold comparison
          if (true_amplitude > avg_background * threshold) {
            // We found a tonal candidate
            tonal_mask[k] = stationarity_weight;

            // 6. Adaptive Walk-Down Width Detection
            // Instead of guessing width, walk down the peak until we hit noise
            // floor or valley.

            const int scan_limit = 50;          // Safety limit
            const float floor_threshold = 1.1f; // Stop if within 10% of floor
            const float slope_threshold =
                1.05f; // Stop if rising (valley) by 5%

            // --- Scan Left ---
            float prev_val = true_amplitude;
            for (int s = 1; s <= scan_limit; s++) {
              int idx = (int)k - s;
              if (idx < 0) {
                break;
              }

              float val = detection_profile[idx];

              // Stop conditions
              if (val <= avg_background * floor_threshold) {
                break; // Hit floor
              }
              if (val > prev_val * slope_threshold) {
                break; // Hit valley/rise
              }

              // Calculate mask weight based on relative energy above floor
              // Normalized height: (val - floor) / (peak - floor)
              // If val is close to peak, weight is close to 1.0 * stationarity
              float relative_height = (val - avg_background) /
                                      (true_amplitude - avg_background + 1e-9f);
              relative_height = fmaxf(0.0f, fminf(1.0f, relative_height));

              // Apply mask
              float mask_val = stationarity_weight * relative_height;
              tonal_mask[idx] = fmaxf(tonal_mask[idx], mask_val);

              prev_val = val;
            }

            // --- Scan Right ---
            prev_val = true_amplitude;
            for (int s = 1; s <= scan_limit; s++) {
              int idx = (int)k + s;
              if (idx >= (int)size) {
                break;
              }

              float val = detection_profile[idx];

              if (val <= avg_background * floor_threshold) {
                break;
              }
              if (val > prev_val * slope_threshold) {
                break;
              }

              float relative_height = (val - avg_background) /
                                      (true_amplitude - avg_background + 1e-9f);
              relative_height = fmaxf(0.0f, fminf(1.0f, relative_height));

              float mask_val = stationarity_weight * relative_height;
              tonal_mask[idx] = fmaxf(tonal_mask[idx], mask_val);

              prev_val = val;
            }
          }
        }
      }
    }
  }
}
