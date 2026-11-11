// Aligns a PureSignal feedback stream with its transmit reference before the
// pair reaches WDSP's pscc().
//
// On ANAN hardware both streams come from DDCs on one clock, tuned by one
// frequency word, so they arrive coherent and with a fixed delay. A FlexRadio
// waveform gets its feedback from a separate receive stream instead, so this
// layer supplies what the hardware gives ANAN for free:
//   - the integer loop delay, found by cross-correlating the envelopes
//     (|tx| and |rx| are immune to any frequency offset), and
//   - a slow phase/frequency tracker that removes the residual offset
//     between the transmit and receive NCOs. Its bandwidth is kept far below
//     the signal's envelope rate so it cannot remove the amplifier's AM/PM,
//     which is what the calibration has to see.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_align pd_align;

// max_delay: the largest loop delay to search, in samples.
// history: how many reference samples to keep; must exceed max_delay plus
// the largest block passed to pd_align_push().
pd_align* pd_align_create(int max_delay, int history);
void pd_align_destroy(pd_align* a);

// Estimates the loop delay from n reference/feedback pairs captured at the
// same instants (interleaved I/Q), to a fraction of a sample: the
// calibration fails outright when the pairs are off by even a quarter
// sample at 24 kHz. Returns the delay in samples, or -1 when the
// correlation peak is not clear enough to trust. n must exceed the largest
// delay by at least 4096 samples.
double pd_align_estimate_delay(pd_align* a, const double* tx, const double* rx, int n);

// The normalized envelope correlation behind the last delay estimate, 0..1.
double pd_align_estimate_corr(const pd_align* a);

// How well the pairs from the last pd_align_push() match: the normalized
// complex correlation of feedback against reference, 0..1. It stays near 1
// with the right delay and drops toward 0 with a wrong one.
double pd_align_coherence(const pd_align* a);

// Delays below PD_ALIGN_MIN_DELAY samples are rounded to whole samples: the
// fractional interpolator needs that much reference ahead of its output.
#define PD_ALIGN_MIN_DELAY 8
void pd_align_set_delay(pd_align* a, double delay);
double pd_align_delay(const pd_align* a);

// Feeds one block: n new reference samples and the n feedback samples
// received over the same interval. Writes n aligned pairs (reference delayed
// by the loop delay, feedback de-rotated) to tx_out/rx_out and returns how
// many are valid; the first `delay` samples after a reset are not.
int pd_align_push(pd_align* a, const double* tx, const double* rx, int n,
                  double* tx_out, double* rx_out);

// Starts a new transmission: forgets the reference history and the pairing,
// keeps the delay and the NCO-offset estimate (both belong to the radio).
void pd_align_restart(pd_align* a);

// Current estimate of the feedback's frequency offset in Hz, given the rate.
double pd_align_offset_hz(const pd_align* a, double rate);

#ifdef __cplusplus
}
#endif
