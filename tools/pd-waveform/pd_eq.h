// Linear equalizer between the waveform's transmit reference and the
// feedback, for PureSignal-style calibration through a radio's own transmit
// filter.
//
// WDSP's calibration assumes the reference is the amplifier's input. On a
// FlexRadio the waveform hands its I/Q to the radio, whose transmit filter
// has frequency-dependent phase; between the reference and the amplifier the
// signal is filtered, so at low amplitude the pairs scatter in phase and
// WDSP's sanity checks reject every fit (measured on a FLEX-8600: c^2+s^2 as
// low as 0.26 on band-limited noise). This fits a short FIR, least squares,
// mapping the aligned reference to the feedback, and runs the reference
// through it (at the reference's own level) so the calibration sees the
// amplifier's real input. The feedback is delayed by the FIR's centre to
// stay aligned.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_eq pd_eq;

// taps: odd, the FIR length.
pd_eq* pd_eq_create(int taps);
void pd_eq_destroy(pd_eq* e);

// Forgets the fit and the training sums.
void pd_eq_reset(pd_eq* e);

// Adds n aligned reference/feedback pairs to the training sums. Train on a
// wideband signal (noise or speech): a two-tone excites two frequencies only.
void pd_eq_train(pd_eq* e, const double* tx, const double* rx, int n);
// Training samples so far.
long pd_eq_trained(const pd_eq* e);

// Solves for the FIR. Returns 1 on success. After it, pd_eq_apply filters.
int pd_eq_solve(pd_eq* e);
int pd_eq_ready(const pd_eq* e);
// Fraction of the feedback's power the linear fit leaves unexplained, from
// the training data (smaller is better; the amplifier's distortion sets it).
double pd_eq_residual(const pd_eq* e);

// Filters n aligned pairs: tx_out is the reference through the FIR, rx_out
// the feedback delayed to match. Returns how many pairs are valid (the first
// taps/2 after a reset are not).
int pd_eq_apply(pd_eq* e, const double* tx, const double* rx, int n, double* tx_out, double* rx_out);

#ifdef __cplusplus
}
#endif
