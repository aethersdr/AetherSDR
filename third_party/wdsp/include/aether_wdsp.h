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
