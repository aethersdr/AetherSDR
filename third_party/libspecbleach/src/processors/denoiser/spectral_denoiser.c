#include "spectral_denoiser.h"
#include "shared/configurations.h"
#include "shared/denoiser_logic/core/denoiser_post_process.h"
#include "shared/denoiser_logic/core/denoiser_profile_core.h"
#include "shared/denoiser_logic/core/noise_floor_manager.h"
#include "shared/denoiser_logic/core/noise_profile.h"
#include "shared/denoiser_logic/estimators/adaptive_noise_estimator.h"
#include "shared/denoiser_logic/estimators/noise_estimator.h"
#include "shared/denoiser_logic/processing/gain_calculator.h"
#include "shared/denoiser_logic/processing/masking_veto.h"
#include "shared/denoiser_logic/processing/suppression_engine.h"
#include "shared/denoiser_logic/processing/tonal_reducer.h"
#include "shared/utils/critical_bands.h"
#include "shared/utils/spectral_features.h"
#include "shared/utils/spectral_smoother.h"
#include "shared/utils/spectral_utils.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

typedef struct SbSpectralDenoiser {
  uint32_t fft_size;
  uint32_t real_spectrum_size;
  uint32_t sample_rate;
  uint32_t hop;
  float default_oversubtraction;
  float default_undersubtraction;

  float* gain_spectrum;
  float* alpha;
  float* beta;
  float* noise_spectrum;
  float* manual_noise_floor;

  TonalReducer* tonal_reducer;

  SpectrumType spectrum_type;
  CriticalBandType band_type;
  DenoiserParameters denoise_parameters;
  GainCalculationType gain_calculation_type;
  TimeSmoothingType time_smoothing_type;
  NoiseEstimatorType noise_estimator_type;

  NoiseEstimator* noise_estimator;
  AdaptiveNoiseEstimator* adaptive_estimator;
  NoiseProfile* noise_profile;
  SpectralFeatures* spectral_features;
  SpectralSmoother* spectrum_smoothing;
  NoiseFloorManager* noise_floor_manager;
  MaskingVeto* masking_veto;
  SuppressionEngine* suppression_engine;

  int last_adaptive_state;
  int last_noise_estimation_method;
  float aggressiveness;
  bool was_learning;
} SbSpectralDenoiser;

SpectralProcessorHandle spectral_denoiser_initialize(
    const uint32_t sample_rate, const uint32_t fft_size,
    const uint32_t overlap_factor, NoiseProfile* noise_profile) {

  if (!noise_profile || sample_rate == 0 || fft_size == 0 ||
      overlap_factor == 0) {
    return NULL;
  }

  SbSpectralDenoiser* self =
      (SbSpectralDenoiser*)calloc(1U, sizeof(SbSpectralDenoiser));
  if (!self) {
    return NULL;
  }

  self->fft_size = fft_size;
  self->real_spectrum_size = (self->fft_size / 2U) + 1U;
  self->hop = self->fft_size / overlap_factor;
  self->sample_rate = sample_rate;
  self->spectrum_type = SPECTRAL_TYPE_1D;
  self->band_type = CRITICAL_BANDS_TYPE_1D;
  self->default_oversubtraction = DEFAULT_OVERSUBTRACTION;
  self->default_undersubtraction = DEFAULT_UNDERSUBTRACTION;
  self->gain_calculation_type = GAIN_ESTIMATION_TYPE_1D;
  self->time_smoothing_type = FIXED;

  self->gain_spectrum = (float*)calloc(self->fft_size, sizeof(float));
  if (!self->gain_spectrum) {
    spectral_denoiser_free(self);
    return NULL;
  }
  (void)initialize_spectrum_with_value(self->gain_spectrum, self->fft_size,
                                       1.F);

  self->alpha = (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->alpha) {
    spectral_denoiser_free(self);
    return NULL;
  }
  (void)initialize_spectrum_with_value(self->alpha, self->real_spectrum_size,
                                       1.F);

  self->beta = (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->beta) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->noise_profile = noise_profile;
  self->noise_spectrum =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->noise_spectrum) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->manual_noise_floor =
      (float*)calloc(self->real_spectrum_size, sizeof(float));
  if (!self->manual_noise_floor) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->tonal_reducer = tonal_reducer_initialize(
      self->real_spectrum_size, self->sample_rate, self->fft_size);
  if (!self->tonal_reducer) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->noise_estimator =
      noise_estimation_initialize(self->fft_size, noise_profile);
  if (!self->noise_estimator) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->spectral_features =
      spectral_features_initialize(self->real_spectrum_size);
  if (!self->spectral_features) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->spectrum_smoothing = spectral_smoothing_initialize(
      self->fft_size, self->sample_rate, self->time_smoothing_type);
  if (!self->spectrum_smoothing) {
    spectral_denoiser_free(self);
    return NULL;
  }

  self->noise_floor_manager = noise_floor_manager_initialize(
      self->fft_size, self->sample_rate, self->hop);

  self->was_learning = false;
  self->aggressiveness = 0.0f;
  self->denoise_parameters.tonal_reduction = 0.0f;

  self->masking_veto = masking_veto_initialize(
      self->fft_size, self->sample_rate, CRITICAL_BANDS_TYPE_1D,
      self->spectrum_type, false, USE_TEMPORAL_MASKING_1D_DEFAULT);
  self->suppression_engine = suppression_engine_initialize(
      self->real_spectrum_size, self->sample_rate, self->band_type,
      self->spectrum_type, true, USE_TEMPORAL_MASKING_1D_DEFAULT);

  if (!self->noise_floor_manager || !self->masking_veto ||
      !self->suppression_engine) {
    spectral_denoiser_free(self);
    return NULL;
  }

  return self;
}

void spectral_denoiser_free(SpectralProcessorHandle instance) {
  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  if (!self) {
    return;
  }

  // Don't free noise profile used as reference here

  if (self->noise_estimator) {
    noise_estimation_free(self->noise_estimator);
  }
  if (self->adaptive_estimator) {
    adaptive_estimator_free(self->adaptive_estimator);
  }
  if (self->spectral_features) {
    spectral_features_free(self->spectral_features);
  }
  if (self->spectrum_smoothing) {
    spectral_smoothing_free(self->spectrum_smoothing);
  }
  if (self->masking_veto) {
    masking_veto_free(self->masking_veto);
  }
  if (self->suppression_engine) {
    suppression_engine_free(self->suppression_engine);
  }
  if (self->gain_spectrum) {
    free(self->gain_spectrum);
  }

  if (self->noise_floor_manager) {
    noise_floor_manager_free(self->noise_floor_manager);
  }
  if (self->alpha) {
    free(self->alpha);
  }
  if (self->beta) {
    free(self->beta);
  }
  if (self->noise_spectrum) {
    free(self->noise_spectrum);
  }
  if (self->manual_noise_floor) {
    free(self->manual_noise_floor);
  }

  if (self->tonal_reducer) {
    tonal_reducer_free(self->tonal_reducer);
  }

  free(self);
}

bool load_reduction_parameters(SpectralProcessorHandle instance,
                               DenoiserParameters parameters) {
  if (!instance) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  // Check if we need to initialize or re-initialize the adaptive estimator
  if (parameters.adaptive_noise) {
    AdaptiveNoiseEstimationMethod requested_method =
        (AdaptiveNoiseEstimationMethod)parameters.noise_estimation_method;

    bool needs_init = !self->adaptive_estimator ||
                      adaptive_estimator_get_method(self->adaptive_estimator) !=
                          requested_method;

    if (needs_init) {
      adaptive_estimator_free(self->adaptive_estimator);
      self->adaptive_estimator = adaptive_estimator_initialize(
          self->real_spectrum_size, self->sample_rate, self->fft_size,
          requested_method);
      self->last_adaptive_state = 0;
    }
  }

  self->denoise_parameters = parameters;
  self->aggressiveness = parameters.aggressiveness;

  return true;
}

bool spectral_denoiser_run(SpectralProcessorHandle instance,
                           float* fft_spectrum) {
  if (!fft_spectrum || !instance) {
    return false;
  }

  SbSpectralDenoiser* self = (SbSpectralDenoiser*)instance;

  // 1. Preparation: Get reference spectrum and handle learning mode
  float* reference_spectrum =
      get_spectral_feature(self->spectral_features, fft_spectrum,
                           self->fft_size, self->spectrum_type);

  if (denoiser_profile_core_handle_learning_mode(
          self->noise_estimator, reference_spectrum,
          self->denoise_parameters.learn_noise, &self->was_learning)) {
    return true;
  }

  // 2. Noise Estimation: Update noise profile (Adaptive or Manual)
  DenoiserProfileCoreParams profile_params = {
      .adaptive_enabled = self->denoise_parameters.adaptive_noise,
      .spectrum_size = self->real_spectrum_size,
      .aggressiveness = &self->aggressiveness,
      .param_aggressiveness = self->denoise_parameters.aggressiveness,
      .last_adaptive_state = &self->last_adaptive_state,
      .adaptive_estimator = self->adaptive_estimator,
      .noise_profile = self->noise_profile,
      .manual_noise_floor = self->manual_noise_floor,
      .noise_spectrum = self->noise_spectrum,
  };
  denoiser_profile_core_update(profile_params, reference_spectrum);

  // 3. Denoising Stage: Calculate gains and apply psychoacoustic constraints

  // 3.1. Calculate SNR-dependent oversubtraction factors (Alpha/Beta)
  SuppressionParameters suppression_params = {
      .type = SUPPRESSION_BEROUTI_PER_BIN,
      .strength = self->denoise_parameters.suppression_strength,
      .undersubtraction = 0.0F};
  suppression_engine_calculate(self->suppression_engine, reference_spectrum,
                               self->noise_spectrum, suppression_params,
                               self->alpha, self->beta);

  // 3.2. Detect tonal components and boost alpha at tonal bins
  tonal_reducer_run(self->tonal_reducer, self->noise_spectrum,
                    get_noise_profile(self->noise_profile, MAX),
                    get_noise_profile(self->noise_profile, MEDIAN), self->alpha,
                    self->denoise_parameters.tonal_reduction);

  // 3.3. Apply temporal smoothing to the input spectrum
  TimeSmoothingParameters spectral_smoothing_parameters =
      (TimeSmoothingParameters){
          .smoothing = self->denoise_parameters.smoothing_factor,
      };
  spectral_smoothing_run(self->spectrum_smoothing,
                         spectral_smoothing_parameters, reference_spectrum);

  // 3.4. Apply Structural Veto to rescue transients and moderate artifacts
  masking_veto_apply(self->masking_veto, reference_spectrum,
                     self->noise_spectrum, NULL, self->alpha,
                     self->denoise_parameters.masking_depth);

  // 3.5. Final Gain Calculation
  calculate_gains(self->real_spectrum_size, self->fft_size, reference_spectrum,
                  self->noise_spectrum, self->gain_spectrum, self->alpha,
                  self->beta, self->gain_calculation_type);

  // 4. Post-Processing: Final gain management and mixing
  DenoiserPostProcessParams post_params = {
      .fft_size = self->fft_size,
      .real_spectrum_size = self->real_spectrum_size,
      .reduction_amount = self->denoise_parameters.reduction_amount,
      .tonal_reduction = self->denoise_parameters.tonal_reduction,
      .whitening_factor = self->denoise_parameters.whitening_factor,
      .residual_listen = self->denoise_parameters.residual_listen,
      .noise_floor_manager = self->noise_floor_manager,
      .tonal_reducer = self->tonal_reducer,
      .gain_spectrum = self->gain_spectrum,
      .noise_spectrum = self->noise_spectrum,
      .fft_spectrum = fft_spectrum,
  };
  denoiser_post_process_apply(post_params);

  return true;
}
