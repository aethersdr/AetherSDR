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
// Local patch: synchronously discard TX rings/filter history and leave stopped.
// Caller excludes exchange/control calls. Returns 0 for RX or a pending fade/flush.
// Retains FFTW plans; flush_iobuffs refreshes its output semaphore.
int DiscardTXAChannelData(int channel);
void fexchange2(int channel, float* inputI, float* inputQ,
                float* outputLeft, float* outputRight, int* error);
void SetRXAMode(int channel, int mode);
// Receive FM construction/control only: these take WDSP's DSP lock and the
// limiter gain setter rebuilds its state. Never call from acquisition/audio.
void SetRXAFMDeviation(int channel, double deviationHz);
void SetRXAFMLimGain(int channel, double maximumGainDb);
void SetRXAFMLimRun(int channel, int run);
void SetRXAPanelGain1(int channel, double gain);
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

// ── FM demodulator deviation ──────────────────────────────────────────────
//
// NO VENDORED PATCH IS INVOLVED, and that is worth saying plainly because the
// opposite was believed about this corner of WDSP for three days.
// `upstream/wdsp.h` is upstream's own GENERATED public interface — its banner
// says it is produced by `gen_wdsp_h.py`, which scans the sources for
// PORT-decorated functions "so that this header cannot drift from what the DLL
// actually exports" — and it is what says whether a WDSP entry point is
// public. It declares `SetRXAFMDeviation`.
//
// THIS SYMBOL WAS NEVER ONE OF THE HIDDEN-LOOKING ONES: `fmd.h` declares it
// too. What misled was a NEIGHBOUR — `fmsq.c` defines `SetRXAFMSQRun` and
// `fmsq.h` never declares it, which reads exactly like a symbol that exists
// and cannot be reached without patching the snapshot. It is not; `wdsp.h`
// declares that one as well. So the rule is about the per-module headers being
// an unreliable NEGATIVE, not about them hiding this call: a per-module header
// saying nothing says nothing. Check `upstream/wdsp.h` before concluding that
// reaching a WDSP symbol needs AETHERSDR-PATCHES.md treatment — that
// discipline is for CHANGING vendored behaviour, which this is not.
//
// DEVIATION IS AN INVERSE AUDIO GAIN, not a bandwidth, and the name misleads
// in a way worth pinning here. `SetRXAFMDeviation` stores the value and
// recomputes `again = rate / (deviation * TWOPI)` (`upstream/fmd.c`); `xfmd`
// then emits `again * (fil_out - fmdc)`. The PLL's capture range is fixed at
// construction (`fmin`/`fmax`, ±8 kHz in `RXA.c`) and this call does not touch
// it, nor the audio filter, nor the de-emphasis. So halving the deviation
// narrows nothing — it DOUBLES the detector's audio output, exactly. The value
// is the receiver's ASSUMPTION about how wide the incoming signal is deviated:
// match the transmission and recovered audio arrives at unity level; assume
// 5 kHz for a 2.5 kHz narrow-FM signal and it arrives 6 dB quiet.
//
// `RXA.c` builds the stage with 5000.0 and, until this declaration existed,
// nothing in this tree could change it — so every FM signal was demodulated
// against a 5 kHz assumption whatever it actually was. 2500 is the European
// narrow-FM figure.
//
// NOTHING NON-LINEAR IS IN THE WAY, which is what makes the effect measurable
// end to end rather than squashed on its way out. `create_fmd` sets
// `lim_run = 0` and nothing in this tree calls `SetRXAFMLimRun`, so the
// detector's own AGC is off and the whole path from `again` to the channel
// output is linear. `wdsp_channel_test`'s ratio assertion depends on that and
// is what will say so if the limiter is ever switched on.
void SetRXAFMDeviation(int channel, double deviationHz);
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

// ── Neural Noise Reduction (NNR, nnr.c — new in WDSP 2.10) ────────────────
//
// A HOST-SIDE STAGE, like the impulse blanker above and unlike everything
// else here. create_nnr()/xnnr()/destroy_nnr() touch neither ch[] nor rxa[],
// so the block runs without a channel — which is what lets AetherSDR run it
// in AudioEngine beside DFNR and RN2 rather than inside the RXA chain, and
// therefore on every backend rather than only the ones that demodulate here
// (RFC #5684 §3.4). RXA builds its own NNR per channel regardless; that one
// is left at run=0 and is not this.
//
// A SPEECH MODEL, trained on HF noise with SSB-processed speech. Measured on
// a 700 Hz carrier it attenuates 28 dB — it classifies an unmodulated tone as
// noise and deletes it. Voice modes only; CW, the digital modes and anything
// on the data path must never reach it.
//
// SAMPLE RATE MUST BE AN INTEGER MULTIPLE OF 16000. calc_nnr() sets run = 0
// and logs otherwise, so a 44.1 kHz path would silently produce no noise
// reduction rather than an error. 48000 gives decim 3.
//
// BUFFER FORMAT is INTERLEAVED DOUBLE — I,Q,I,Q — `size` complex samples,
// same as the blanker. Only I is read. On output, Q is zeroed when cmode is
// 1 and mirrors I when it is 0. The same pointer may not be passed as both
// in and out: the block delays its output through an internal FIFO.
//
// LATENCY is 51.17 ms at 48 kHz, and getDelay_nnr() reports it in samples.
// That is less than WDSP's own spectral NR (64 ms). Both models have the same
// latency — the larger one costs processor time, not delay.
//
// SIZE AND RATE ARE FIXED AT CONSTRUCTION in practice. setSize_nnr() and
// setSamplerate_nnr() tear the block down and rebuild it, which reallocates
// ~17 buffers, re-plans two FFTW_PATIENT transforms and re-deserialises both
// trained models. Control-path work only — never from an audio callback.
//
// COSTS, measured: ~13% of one modern x86 P-core for the Standard model and
// ~29% for Premium, per instance, plus ~9 MB RSS because each instance copies
// both models' weights into its own double blob.
//
// LOCKING IS THE CALLER'S. The setters below are the RXA properties without
// their ch[channel].csDSP section (AETHERSDR-PATCHES.md patch 5), so a host
// that drives them from a GUI thread while xnnr() runs on an audio thread
// must serialise them itself.
//
// MODEL FILES: every construction looks for wdsp_nnr_0.bin and wdsp_nnr_1.bin
// in the PROCESS'S WORKING DIRECTORY and prefers either over the built-in
// copy. A well-formed model with the wrong dimensions leaves that slot
// passing audio through untouched. Kept deliberately (RFC #5684 §8);
// third_party/wdsp/README.md has the measured behaviour.
typedef struct _nnr* NNR;

// run: 0 bypasses (a straight copy when in != out). position: which xnnr()
// call site acts, for hosts that call it from more than one. size: complex
// samples per xnnr() call. rate: must be a multiple of nrate. nrate/fftsize/
// overlap/lookahead: 16000/512/2/1 are what RXA uses and what the models were
// trained for. mask_floor: how far one bin may be attenuated, −10 (most noise
// passed) to −50 (maximum suppression), default −25. cmode: 1 zeroes Q.
NNR create_nnr(int run, int position, int size, double* in_buff, double* out_buff,
               int rate, int nrate, int fftsize, int overlap, int lookahead,
               double mask_floor, int cmode);
void destroy_nnr(NNR a);
// Runs only when the block's own position matches `pos`; otherwise copies.
void xnnr(NNR a, int pos);
// Clears the FIFO, the overlap-add state and the network's recurrent state.
void flush_nnr(NNR a);
void setBuffers_nnr(NNR a, double* in, double* out);
// Both rebuild the block — control path only. See the note above.
void setSamplerate_nnr(NNR a, int rate);
void setSize_nnr(NNR a, int size);
// Algorithmic delay in samples at the dsp rate.
int getDelay_nnr(NNR a);
int getRun_nnr(NNR a);
// 0 = Standard, 1 = Premium. Both are compiled in and both are loaded, so a
// switch takes effect within a frame. RETURNS THE SLOT ACTUALLY IN USE, which
// differs from the request when that slot holds no model — the documented way
// to discover what a build contains instead of assuming.
int setModel_nnr(NNR a, int slot);
int getModel_nnr(NNR a);

// The control surface. setMaskFloor_nnr is the one control the WDSP Guide
// intends operators to touch; the rest are undocumented tuning that AetherSDR
// exposes by decision (RFC #5684 §8). src/core/NnrControls.h carries each
// one's range and the value WDSP starts it at.
void setRun_nnr(NNR a, int run);
void setPosition_nnr(NNR a, int position);
void setCmode_nnr(NNR a, int cmode);
void setMaskFloor_nnr(NNR a, double floor_db);
// 0 = network, 1 = identity, 2 = lowpass. Debug; not an operator control.
void setTestMode_nnr(NNR a, int mode);
void setAlpha_nnr(NNR a, double alpha);
void setAlphaKnee_nnr(NNR a, double knee_db);
void setTau_nnr(NNR a, double tau);
void setMaxGain_nnr(NNR a, double gmax_db);
void setSmooth_nnr(NNR a, double att_ms, double rel_ms);

// ── Display analyzer (analyzer.c) ─────────────────────────────────────────
//
// WDSP's panadapter engine: windowed FFT, per-pixel detector, time averaging
// and conversion to dB, computed on WDSP-owned worker threads. `disp` is a
// slot in [0, 72) — its own namespace, not a channel id, though a host may
// reuse one as the other.
//
// XCreateAnalyzer sizes the buffers for FFTs up to `maxSize`; the input ring is
// 2 * maxSize samples, so `maxWriteahead` passed to SetAnalyzer must stay below
// that, and `bufferSize` must divide it. SetAnalyzer plans with FFTW_PATIENT:
// call it off any real-time thread and under the host's FFTW planner lock.
//
// Spectrum0 takes exactly `bufferSize` complex samples, INTERLEAVED DOUBLE,
// and reads element 2i+1 as I and 2i as Q — the swap mirrors the spectrum.
//
// GetPixels copies `numPixels` floats (dB) into `pixels` and sets *flag = 1
// when a frame newer than the last call exists, else sets *flag = 0.
//
// Detector modes: 0 peak, 1 Rosenfell, 2 average, 3 sample, 4 RMS.
// Average modes: -1 peak hold, 0 none, 1 linear recursive, 2 linear window,
// 3 log recursive.
void XCreateAnalyzer(int disp, int* success, int maxSize, int maxNumFft,
                     int maxStitch, char* appDataPath);
void DestroyAnalyzer(int disp);
void SetAnalyzer(int disp, int numPixout, int numFft, int complexInput,
                 int* highSideLo, int fftSize, int bufferSize, int windowType,
                 double kaiserPiAlpha, int overlap, int clipBins,
                 double clipLowBins, double clipHighBins, int numPixels,
                 int numStitch, int calibrationSet, double fMin, double fMax,
                 int maxWriteahead);
void Spectrum0(int run, int disp, int ss, int LO, double* pbuff);
void GetPixels(int disp, int pixout, float* pixels, int* flag);
void ResetPixelBuffers(int disp);
void SetDisplayDetectorMode(int disp, int pixout, int mode);
void SetDisplayAverageMode(int disp, int pixout, int mode);
void SetDisplayNumAverage(int disp, int pixout, int num);
void SetDisplayAvBackmult(int disp, int pixout, double mult);
void SetDisplaySampleRate(int disp, int rate);
void SetDisplayNormOneHz(int disp, int pixout, int norm);

int GetWDSPVersion(void);

uint64_t wdspPortThreadAllocationSequence(void);
uint64_t wdspPortAllocationSequence(void);
uint64_t wdspPortOutstandingAllocations(void);

// TEST ONLY (AetherSDR patch 13). Makes the DSP worker sleep for this long
// immediately after dexchange() has released the host's blocked fexchange*,
// which is the window in which #5734's input overwrite happened. 0 (the
// default) is a single relaxed load and no sleep. Process-global.
void wdspPortSetHandoffPauseForTest(unsigned microseconds);

#ifdef __cplusplus
}
#endif
