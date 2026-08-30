
#define DEEP_FILTER_MAJOR 0
#define DEEP_FILTER_MINOR 5
#define DEEP_FILTER_PATCH 7


#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

struct DFState;

extern "C" {

/// Create a DeepFilterNet Model
///
/// Args:
///     - path: File path to a DeepFilterNet tar.gz onnx model
///     - atten_lim: Attenuation limit in dB.
///
/// Returns:
///     - DF state doing the full processing: stft, DNN noise reduction, istft.
DFState *df_create(const char *path, float atten_lim, const char *log_level);

/// Get DeepFilterNet frame size in samples.
uintptr_t df_get_frame_length(DFState *st);

/// Get the next log message. Must be freed via `df_free_log_msg(ptr)`
char *df_next_log_msg(DFState *st);

void df_free_log_msg(char *ptr);

/// Set DeepFilterNet attenuation limit.
///
/// Args:
///     - lim_db: New attenuation limit in dB.
void df_set_atten_lim(DFState *st, float lim_db);

/// Set DeepFilterNet post filter beta. A beta of 0 disables the post filter.
///
/// Args:
///     - beta: Post filter attenuation. Suitable range between 0.05 and 0;
void df_set_post_filter_beta(DFState *st, float beta);

/// Processes a chunk of samples.
///
/// Args:
///     - df_state: Created via df_create()
///     - input: Input buffer of length df_get_frame_length()
///     - output: Output buffer of length df_get_frame_length()
///
/// Returns:
///     - Local SNR of the current frame.
float df_process_frame(DFState *st, float *input, float *output);

/// Processes a filter bank sample and return raw gains and DF coefs.
///
/// Args:
///     - df_state: Created via df_create()
///     - input: Spectrum of shape `[n_freqs, 2]`.
///     - out_gains_p: Output buffer of real-valued ERB gains of shape `[nb_erb]`. This function
///         may set this pointer to NULL if the local SNR is greater 30 dB. No gains need to be
///         applied then.
///     - out_coefs_p: Output buffer of complex-valued DF coefs of shape `[df_order, nb_df_freqs, 2]`.
///         This function may set this pointer to NULL if the local SNR is greater 20 dB. No DF
///         coefficients need to be applied.
///
/// Returns:
///     - Local SNR of the current frame.
float df_process_frame_raw(DFState *st,
                           float *input,
                           float **out_gains_p,
                           float **out_coefs_p);

/// Free a DeepFilterNet Model
void df_free(DFState *model);

}  // extern "C"
