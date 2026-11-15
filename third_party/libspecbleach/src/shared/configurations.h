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

#ifndef MODULES_CONFIGURATIONS_H
#define MODULES_CONFIGURATIONS_H

#include "utils/spectral_utils.h"
#include <stdbool.h>
#include <stdint.h>

// MSVC does not support _Static_assert in C mode — map to static_assert
#if defined(_MSC_VER) && !defined(__cplusplus)
#define _Static_assert static_assert
#endif

// Compile-time assertions for configuration validity
_Static_assert(HANN_WINDOW >= 0 && HANN_WINDOW <= 3,
               "HANN_WINDOW must be between 0 and 3");
_Static_assert(HAMMING_WINDOW >= 0 && HAMMING_WINDOW <= 3,
               "HAMMING_WINDOW must be between 0 and 3");
_Static_assert(BLACKMAN_WINDOW >= 0 && BLACKMAN_WINDOW <= 3,
               "BLACKMAN_WINDOW must be between 0 and 3");
_Static_assert(VORBIS_WINDOW >= 0 && VORBIS_WINDOW <= 3,
               "VORBIS_WINDOW must be between 0 and 3");

// Additional C17 compile-time validations
_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be exactly 32 bits");

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#ifndef M_PIf
#define M_PIf (3.14159265358979323846F)
#endif

/* --------------------------------------------------------------------- */
/* ------------------- Shared Modules configurations ------------------- */
/* --------------------------------------------------------------------- */

#define SPECTRAL_EPSILON (1e-12F)
#define MAX_SPECTRAL_CIRCULAR_BUFFER_LAYERS 8

// Absolute hearing thresholds
#define REFERENCE_SINE_WAVE_FREQ (1000.F)
#define REFERENCE_LEVEL (90.F)
#define SINE_AMPLITUDE (1.F)

// Johnston Psychoacoustic Model (1988)
#define DB_FS_TO_SPL_REF 96.0F    // 0dBFS -> 96dB SPL reference level
#define POWER_LAW_EXPONENT (0.6F) // Johnston power-law integration exponent

// Default Additivity Exponents
#define SPECTRAL_ADDITIVITY_EXPONENT_STANDARD (1.0F) // Pure Johnston
#define SPECTRAL_ADDITIVITY_EXPONENT_PEAQ (0.4F)     // Advanced precision

// Schroeder spreading function constants
#define S_MIN_UPWARD (5.0F)  // Minimum upward spreading slope (dB/Bark)
#define S_MAX_UPWARD (15.0F) // Maximum upward spreading slope (dB/Bark)
#define S_DOWNWARD (25.0F)   // Constant downward spreading slope (dB/Bark)

// Threshold Offsets (NMT = Noise-Masking-Tone, TMN = Tone-Masking-Noise)
#define NMT_OFFSET_DB (6.0F)    // Standard offset for NMT
#define TMN_OFFSET_BASE (14.5F) // Base offset for TMN (Bark-dependent)

// Johnston SFM (Spectral Flatness Measure) constants
#define SFM_MIN_DB (-60.0F) // Minimum expected SFM (highly tonal)
#define SFM_MAX_DB (0.0F)   // Maximum expected SFM (random noise)
// Temporal Masking Constants
#define FORWARD_MASKING_TAU_LOW_MS (0.100F)  // 100ms decay for low frequencies
#define FORWARD_MASKING_TAU_HIGH_MS (0.025F) // 25ms decay for high frequencies
#define BACKWARD_MASKING_TAU_MS (0.010F)     // 10ms decay for pre-masking

// Schroeder Slope Adaptation Constants
#define S_LEVEL_REF_DB 40.0F // Reference level for slope adaptation (dB SPL)
#define S_SLOPE_FACTOR 0.2F  // Slope broadening factor per dB

// Veto Parameters
#define MASKING_VETO_SMOOTHING 0.5F  // Stabilization alpha for clean signal
#define MASKING_VETO_NMR_RANGE 20.0F // NMR range for protection mapping (dB)

// Gain Estimators
#define GSS_EXPONENT                                                           \
  2.0F // 2 Power Subtraction / 1 Magnitude Subtraction / 0.5 Spectral
       // Subtraction

// Oversubtraction criteria
#define ALPHA_MAX (4.F)
#define ALPHA_MAX_TONAL (10.F)
#define ALPHA_MIN (1.F)
#define DEFAULT_OVERSUBTRACTION (ALPHA_MIN)
#define DEFAULT_UNDERSUBTRACTION (0.01F)
#define SUPPRESSION_LOWER_SNR_DB (-5.0F)
#define SUPPRESSION_HIGHER_SNR_DB (20.0F)

// Adaptive Estimator
#define ESTIMATOR_SILENCE_THRESHOLD (1e-10F) // Roughly -100dB in power

// Martin (2001) Constants
#define MARTIN_SUBWIN_COUNT 8 // Number of sub-windows
#define MARTIN_SUBWIN_LEN 12  // Sub-window length (96/8)
#define MARTIN_BIAS_CORR 1.5F // Conservative bias correction for min tracking
#define MARTIN_SMOOTH_ALPHA 0.8F // Baseline smoothing for PSD

// SPP-MMSE Estimator Constants
#define SPP_PRIOR_H1 (0.5F)      // P(H1) - Speech present prior
#define SPP_PRIOR_H0 (0.5F)      // P(H0) - Speech absent prior
#define SPP_FIXED_XI_H1 (31.62F) // Fixed a priori SNR (15 dB in linear)
#define SPP_ALPHA_POW (0.8F)     // Power spectrum smoothing factor
#define SPP_SMOOTH_SPP (0.9F)    // SPP smoothing for stagnation control
#define SPP_CURRENT_SPP (0.1F)   // Current SPP weighting for stagnation control
#define SPP_STAGNATION_CAP (0.99F) // Maximum SPP value to prevent locking

// Brandt (Trimmer Mean) Constants
#define BRANDT_DEFAULT_HISTORY_MS 5000.0f
#define BRANDT_DEFAULT_PERCENTILE 0.5f
#define BRANDT_MIN_CONFIDENCE                                                  \
  0.90f // Lowered from 0.98 for better learning speed
#define BRANDT_ESTIMATOR_BIAS_EPSILON                                          \
  (1e-6F) // Precision for bias correction calc
#define BRANDT_ESTIMATOR_MIN_HISTORY_FRAMES                                    \
  5U // Minimum frames for history-based tracking
#define BRANDT_ESTIMATOR_MIN_DURATION_MS                                       \
  (0.1F) // Safety floor for duration calcs

// Tonal Detector Constants
#define PEAK_THRESHOLD 1.41f        // ~3dB above neighbor background
#define STATIONARITY_THRESHOLD 2.5f // Ratio of Max/Median spread
#define LOW_FREQ_HZ 200.0f
#define MID_FREQ_HZ 1000.0f
#define BG_RADIUS_LOW 15
#define BG_RADIUS_HIGH 7
#define THRESHOLD_FACTOR_LOW 0.85f
#define THRESHOLD_FACTOR_HIGH 1.0f
#define MAX_NEIGHBORS 30

// Transient Detector Constants
#define UPPER_LIMIT (5.F)
#define DEFAULT_TRANSIENT_THRESHOLD (2.F)
#define MIN_INNOVATION_ENERGY 1e-10F  // ~ -100dB floor for transient trigger
#define ONSET_RATIO_SENSITIVITY 0.25F // Innovation required for full weight
#define TRANSIENT_SMOOTH_ALPHA 0.8F   // Reference smoothing alpha

// Noise Estimator Constants
#define MIN_NUMBER_OF_WINDOWS_NOISE_AVERAGED 5
#define NUMBER_OF_MEDIAN_SPECTRUM 25
#define NOISE_ESTIMATION_INTERPOLATION_THRESHOLD (1e-9F)
#define NOISE_ESTIMATION_SMOOTHING_FACTOR (0.5F)
#define ADAPTIVE_NOISE_FLOOR_SMOOTHING (0.5F)

// NLM (Lukin Algorithm B) Parameters
#define NLM_PATCH_SIZE 8U
#define NLM_PASTE_BLOCK_SIZE 4U
#define NLM_SEARCH_RANGE_FREQ 8U
#define NLM_SEARCH_RANGE_TIME_PAST 16U
#define NLM_SEARCH_RANGE_TIME_FUTURE 4U
#define NLM_DEFAULT_H_PARAMETER 1.0F
#define DELAY_BUFFER_FRAMES (NLM_SEARCH_RANGE_TIME_PAST + 1U)
#define NLM_MIN_WEIGHT 1e-10F

/* --------------------------------------------------------------- */
/* ------------------- 1D Denoiser configurations ---------------- */
/* --------------------------------------------------------------- */

// STFT configurations
#define OVERLAP_FACTOR_1D 4
#define INPUT_WINDOW_TYPE_1D HANN_WINDOW
#define OUTPUT_WINDOW_TYPE_1D HANN_WINDOW

// Fft configuration
#define PADDING_CONFIGURATION_1D FIXED_AMOUNT
#define ZEROPADDING_AMOUNT_1D 800

// Spectral Type
#define SPECTRAL_TYPE_1D POWER_SPECTRUM

// Noise Scaling strategy
#define CRITICAL_BANDS_TYPE_1D BARK_SCALE
#define GAIN_ESTIMATION_TYPE_1D WIENER

// Masking Veto defaults
#define USE_TEMPORAL_MASKING_1D_DEFAULT true

/* ------------------------------------------------------------------ */
/* ------------------- 2D Denoiser configurations ------------------- */
/* ------------------------------------------------------------------ */

// STFT configurations
#define OVERLAP_FACTOR_2D 4
#define INPUT_WINDOW_TYPE_2D HANN_WINDOW
#define OUTPUT_WINDOW_TYPE_2D HANN_WINDOW

// Fft configuration
#define PADDING_CONFIGURATION_2D FIXED_AMOUNT
#define ZEROPADDING_AMOUNT_2D 800

// Spectral Type
#define SPECTRAL_TYPE_2D POWER_SPECTRUM

// Noise Scaling strategy
#define CRITICAL_BANDS_TYPE_2D BARK_SCALE
#define GAIN_ESTIMATION_TYPE_2D WIENER

// Masking Veto defaults
#define USE_TEMPORAL_MASKING_2D_DEFAULT true

#endif // MODULES_CONFIGURATIONS_H
