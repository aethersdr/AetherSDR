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
