// PureSignal predistortion core for an on-radio waveform.
//
// Wraps WDSP's own PureSignal stages, calcc (calibration) and iqc
// (correction), without the rest of WDSP: no FFTW and no transmit channel.
// That keeps the binary small enough for a FLEX-6000 legacy waveform. The
// one thing those stages need from WDSP's transmit channel, the txa[] slot
// holding their pointers and lock, is supplied here.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_core ps_core;

// rate: sample rate of both the transmit stream and the feedback, in Hz.
// block: the largest number of samples passed to ps_core_correct().
// peak: the largest transmit amplitude the waveform produces, which WDSP maps
// to the top of its calibration range.
ps_core* ps_core_create(int rate, int block, double peak);
void ps_core_destroy(ps_core* c);

// Pre-distorts n transmit samples in place (interleaved I/Q). Before the
// first calibration has succeeded this leaves them unchanged.
void ps_core_correct(ps_core* c, double* iq, int n);

// Feeds n aligned reference/feedback pairs to the calibration.
void ps_core_calibrate(ps_core* c, const double* tx, const double* rx, int n);

// Tells the calibration whether the radio is transmitting. It only collects
// while keyed, and skips the first 0.1 s after keying.
void ps_core_set_keyed(ps_core* c, int keyed);

// Drops every learned correction and starts over.
void ps_core_reset(ps_core* c);

// 1 while a correction is applied.
int ps_core_correcting(const ps_core* c);
// WDSP's calibration state (LRESET..LTURNON) and its fit-result flags.
int ps_core_state(const ps_core* c);
int ps_core_fit_flags(const ps_core* c);
// 1 while a new correction set waits for ps_core_correct() to cross-fade it
// in. The calibration thread blocks until that happens, so the transmit path
// must keep running.
int ps_core_swap_pending(const ps_core* c);
// How many fits have completed.
int ps_core_fits(const ps_core* c);

#ifdef __cplusplus
}
#endif
