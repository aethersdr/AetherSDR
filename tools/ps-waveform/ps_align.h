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

typedef struct ps_align ps_align;

// max_delay: the largest loop delay to search, in samples.
// history: how many reference samples to keep; must exceed max_delay plus
// the largest block passed to ps_align_push().
ps_align* ps_align_create(int max_delay, int history);
void ps_align_destroy(ps_align* a);

// Estimates the loop delay from n reference/feedback pairs captured at the
// same instants (interleaved I/Q), to a fraction of a sample: the
// calibration fails outright when the pairs are off by even a quarter
// sample at 24 kHz. Returns the delay in samples, or -1 when the
// correlation peak is not clear enough to trust.
double ps_align_estimate_delay(const ps_align* a, const double* tx, const double* rx, int n);

// Delays below PS_ALIGN_MIN_DELAY samples are rounded to whole samples: the
// fractional interpolator needs that much reference ahead of its output.
#define PS_ALIGN_MIN_DELAY 8
void ps_align_set_delay(ps_align* a, double delay);
double ps_align_delay(const ps_align* a);

// Feeds one block: n new reference samples and the n feedback samples
// received over the same interval. Writes n aligned pairs (reference delayed
// by the loop delay, feedback de-rotated) to tx_out/rx_out and returns how
// many are valid; the first `delay` samples after a reset are not.
int ps_align_push(ps_align* a, const double* tx, const double* rx, int n,
                  double* tx_out, double* rx_out);

// Current estimate of the feedback's frequency offset in Hz, given the rate.
double ps_align_offset_hz(const ps_align* a, double rate);

#ifdef __cplusplus
}
#endif
