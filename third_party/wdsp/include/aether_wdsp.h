#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum AetherWdspRxMode
{
    AETHER_WDSP_RX_LSB = 0,
    AETHER_WDSP_RX_USB = 1,
    AETHER_WDSP_RX_DSB = 2,
    AETHER_WDSP_RX_CWL = 3,
    AETHER_WDSP_RX_CWU = 4,
    AETHER_WDSP_RX_FM = 5,
    AETHER_WDSP_RX_AM = 6,
    AETHER_WDSP_RX_DIGU = 7,
    AETHER_WDSP_RX_SPEC = 8,
    AETHER_WDSP_RX_DIGL = 9,
    AETHER_WDSP_RX_SAM = 10,
    AETHER_WDSP_RX_DRM = 11,
    AETHER_WDSP_RX_WBFM = 12
};

void OpenChannel(int channel, int inputSize, int dspSize, int inputSampleRate,
                 int dspSampleRate, int outputSampleRate, int type, int state,
                 double delayUp, double slewUp, double delayDown,
                 double slewDown, int blockForOutput);
void CloseChannel(int channel);
// Channel run state. state 1 = running, 0 = stopped. dmode 1 makes a stop
// BLOCK until the channel has flushed (bounded by WDSP's own 100 ms timeout),
// which is what makes it safe to tear down or reconfigure behind it; dmode 0
// returns immediately. Returns the prior state, so callers can restore it.
int SetChannelState(int channel, int state, int dmode);
void fexchange2(int channel, float* inputI, float* inputQ,
                float* outputLeft, float* outputRight, int* error);
void SetRXAMode(int channel, int mode);
void SetRXABandpassFreqs(int channel, double lowHz, double highHz);
// Canonical passband setter. RXASetPassband() is what both reference clients
// (Thetis, pihpsdr) call: it sets the bandpass AND the SNBA output bandwidth
// AND the NBP stage. SetRXABandpassFreqs() alone touches only the first, which
// leaves the filter actually in circuit untouched — no sideband selection, so
// USB and LSB demodulate identically and filter edges have no audible effect.
void RXASetPassband(int channel, double lowHz, double highHz);
// The two stages RXASetPassband sets in addition to the bandpass, exposed
// separately so their effects can be attributed independently.
void RXANBPSetFreqs(int channel, double lowHz, double highHz);
void SetRXASNBAOutputBandwidth(int channel, double lowHz, double highHz);
// RX frequency shift. Lets a single-DDC backend hold its NCO (and therefore the
// panadapter centre) still while the slice tunes within the passband.
void SetRXAShiftFreq(int channel, double shiftHz);
void SetRXAShiftRun(int channel, int run);

// ── Impulse noise blanker (ANB, nob.c) ────────────────────────────────────
//
// NOT part of the RXA chain, and that is a property of WDSP rather than a
// choice made here: RXA.c never instantiates an ANB, so unlike the AGC or the
// notches there is no SetRXAANB* to call. The blanker is a stage the HOST runs
// on raw IQ before handing it to the channel — which is also the only place it
// CAN run, because blanking an impulse has to happen before any filter smears
// it across time. Both reference clients do exactly this (pihpsdr
// receiver.c full_rx_buffer, Thetis cmaster.c).
//
// IDENTIFIED BY AN `id`, from a table of 32 in nob.c that is INDEPENDENT of
// WDSP's channel table. This host deliberately passes the WdspChannel's own
// channel id as the blanker id: both tables are 32 entries, WdspChannel already
// owns the allocation and release of that number, and reusing it means one
// lifetime instead of two that can drift apart. Anything else linking against
// this facade must do the same, or claim ids from a separate allocator.
//
// BUFFER FORMAT is INTERLEAVED DOUBLE — I,Q,I,Q — `buffsize` complex samples
// long, and the same pointer may be passed as both in and out for in-place
// operation. This is fexchange0's format, not fexchange2's separate float
// planes, so a host on fexchange2 has to interleave into a staging buffer.
//
// HANDEDNESS DOES NOT MATTER. xanb() looks only at sqrt(I*I + Q*Q), so a
// conjugated wire (the HPSDR convention — see Hl2RxDsp::processIqBlock) blanks
// identically to an unconjugated one. This is the one IQ stage on this path
// with no sign trap in it.
//
// PARAMETERS. `threshold` is the trigger: a sample blanks when its magnitude
// exceeds threshold times the running average magnitude, so SMALLER is MORE
// aggressive. tau/hangtime/advtime are the transition, hold-off and look-ahead
// times in seconds; backtau is the time constant of that running average.
// pihpsdr's fixed set is (0.0001, 0.0001, 0.0001, 0.05, 20).
//
// ARMING DELAY, which is not obvious and is worth designing around: flush
// (and creation) sets the running average to 1.0 — full scale — so immediately
// afterwards nothing exceeds threshold * avg and the blanker does nothing until
// the average has decayed to the real signal level. At backtau = 0.05 s that is
// a few hundred milliseconds. The failure direction is the safe one (no
// blanking rather than blanking everything), but a host that flushes on every
// transmit-to-receive edge gets an unarmed blanker for the first moment of
// every receive period.
//
// Every Set* below is control-path work: they take the stage's lock. NONE of
// them allocate — create_anb sizes the delay line once at
// (MAX_TAU + MAX_ADVTIME) * MAX_SAMPLERATE, independent of both buffsize and
// samplerate, so SetEXTANBBuffsize only stores the count and
// SetEXTANBSamplerate only re-derives the sample counts through initBlanker().
//
// xanbEXT allocates nothing and is safe on the real-time path. flush_anbEXT
// allocates nothing either, but it is CHEAP rather than FREE: it takes the same
// lock as the setters and initBlanker() memsets the whole delay line (~98 KB)
// and rebuilds the transition table. Calling it from the audio callback is
// permissible; calling it on every block, or on every transmit-to-receive edge,
// is not what it is for — see the ARMING DELAY note above.
void create_anbEXT(int id, int run, int buffsize, double samplerate, double tau,
                   double hangtime, double advtime, double backtau,
                   double threshold);
void destroy_anbEXT(int id);
// Resets the state machine and the delay line, and re-arms as described above.
void flush_anbEXT(int id);
// In-place safe: pass the same pointer for both.
void xanbEXT(int id, double* in, double* out);
void SetEXTANBRun(int id, int run);
void SetEXTANBBuffsize(int id, int size);
void SetEXTANBSamplerate(int id, int rate);
void SetEXTANBTau(int id, double tau);
void SetEXTANBHangtime(int id, double time);
void SetEXTANBAdvtime(int id, double time);
void SetEXTANBBacktau(int id, double tau);
void SetEXTANBThreshold(int id, double thresh);

// ── Manual notch filters (the notched-bandpass stage, nbp0) ────────────────
//
// This is the host-side equivalent of a Flex TNF, and on a direct-sampling
// radio it is the ONLY notch there is: the HL2 protocol carries no DSP at all
// (HL2 oracle addendum 3 §B4), so a notch either happens here or not at all.
//
// COORDINATE SPACE — the one thing to get right. Notch centres are ABSOLUTE
// frequencies in the same units the host feeds RXANBPSetTuneFrequency(); WDSP
// subtracts (tunefreq + shift) internally when it rebuilds the filter mask
// (nbp.c calc_nbp_lightweight). Feed it RF Hz and a notch stays glued to the
// interferer as the operator tunes — which is what makes it a *tracking*
// notch rather than an audio-frequency one. Feed it baseband offsets and it
// will appear to work until the moment someone tunes.
//
// A host that calls SetRXAShiftFreq() MUST also call RXANBPSetShiftFrequency()
// with the matching value; nothing inside WDSP connects the two, so the notch
// silently drifts off the signal by the shift amount otherwise.
//
// The notch handle is a POSITIONAL INDEX into a dense array, not a stable id.
// Add inserts at `notch` and shifts everything above it up; Delete closes the
// gap and shifts everything above it down. A caller holding its own stable ids
// must remap after every delete or it will edit the wrong notch.
//
// Width has a floor that depends on the filter length, and it is enforced
// SILENTLY: the nbp stage is created with autoincr=1 (RXA.c), so a notch
// narrower than min_notch_width() is widened rather than refused. That floor is
// 1600 / (nc / 256) * (rate / 48000) Hz for the default window type — 200 Hz at
// nc=2048, 50 Hz at nc=8192. Ask for 50 Hz at 2048 taps and WDSP gives you 200
// without saying so, which is the difference between notching one carrier and
// notching the carrier plus everything around it.
//
// Returns 0 on success and -1 when `notch` is out of range (Add also fails once
// the database is full; RXA.c creates it with room for 1024).
int RXANBPAddNotch(int channel, int notch, double fcenterHz, double fwidthHz,
                   int active);
int RXANBPEditNotch(int channel, int notch, double fcenterHz, double fwidthHz,
                    int active);
int RXANBPDeleteNotch(int channel, int notch);
int RXANBPGetNotch(int channel, int notch, double* fcenterHz, double* fwidthHz,
                   int* active);
void RXANBPGetNumNotches(int channel, int* nnotches);
// The tuned frequency notch centres are measured against. Push this on every
// NCO change.
void RXANBPSetTuneFrequency(int channel, double tunefreqHz);
// The companion to SetRXAShiftFreq — see the coordinate-space note above.
void RXANBPSetShiftFrequency(int channel, double shiftHz);
// Global enable for every notch on the channel, equivalent to a Flex
// tnf_enabled. Individual notches keep their own `active` flag underneath it.
void RXANBPSetNotchesRun(int channel, int run);
// Bandpass filter length and minimum-phase mode. Composite calls, like
// RXASetPassband: RXASetNC also stops and restarts the channel.
void RXASetNC(int channel, int nc);
void RXASetMP(int channel, int mp);
void SetRXAAGCMode(int channel, int mode);
void SetRXAAGCTop(int channel, double maximumGainDb);
// The rest of the AGC surface. SetRXAAGCMode alone leaves slope and the time
// constants at WDSP's defaults; pihpsdr sets all of them (receiver.c set_agc).
// Slope is the output difference between very weak and very strong signals —
// at 0 it is maximum compression, which lifts the noise floor to the ceiling.
void SetRXAAGCSlope(int channel, int slope);
void SetRXAAGCFixed(int channel, double fixedGainDb);
void SetRXAAGCAttack(int channel, int attackMs);
void SetRXAAGCDecay(int channel, int decayMs);
void SetRXAAGCHang(int channel, int hangMs);
void SetRXAAGCHangThreshold(int channel, int hangThreshold);
void SetTXAMode(int channel, int mode);
void SetTXABandpassFreqs(int channel, double lowHz, double highHz);
// RXA meter readouts. RXA_S_PK / RXA_S_AV are the real signal-strength
// meters. RXA_ADC_PK / RXA_ADC_AV measure the POST-DDC slice, which is a
// different question from the HL2's own pre-DDC full-spectrum clip
// indicator — they can disagree completely and both are worth showing.
enum AetherWdspRxMeter
{
    AETHER_WDSP_RXA_S_PK = 0,
    AETHER_WDSP_RXA_S_AV = 1,
    AETHER_WDSP_RXA_ADC_PK = 2,
    AETHER_WDSP_RXA_ADC_AV = 3,
    AETHER_WDSP_RXA_AGC_GAIN = 4
};
double GetRXAMeter(int channel, int meterType);

int GetWDSPVersion(void);

uint64_t wdspPortAllocationSequence(void);
uint64_t wdspPortOutstandingAllocations(void);

#ifdef __cplusplus
}
#endif
