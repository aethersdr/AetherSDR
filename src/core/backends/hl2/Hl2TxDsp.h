#pragma once

#include "core/dsp/WdspChannel.h"

#include <QObject>

#include <complex>
#include <string>
#include <vector>

namespace AetherSDR::hl2 {

// SSB transmit chain for the Hermes-Lite 2: processed TX audio in, baseband IQ
// out, ready for EP2.
//
// The audio arrives already shaped — AudioEngine's TX chain has applied the test
// tone, compressor and EQ before we see it — so this stage is only modulation.
// That is deliberate: it means the TONE button, the microphone and any future
// source all reach the air through ONE path, and what the operator monitors is
// what gets transmitted.
//
// RATES. AudioEngine runs at 24 kHz; EP2 is clocked at a fixed 48 kHz
// regardless of the RX sample rate. WDSP's three-rate channel model does the
// interpolation, which is the same mechanism the RX side uses in the opposite
// direction rather than a second, hand-rolled resampler.
//
// MODULATION is a phasing SSB modulator built here rather than WDSP's TXA
// chain. WDSP's transmit path WORKS — wdsp_channel_test proves it — but driven
// from this backend's configuration it returned underruns and zeros, and the
// failure mode is silent. See the long note in the .cpp.
//
// The output is CONJUGATED for the HPSDR wire, which has the opposite handedness
// to the standard analytic convention. Omitting that transmitted every signal on
// the wrong sideband.
class Hl2TxDsp : public QObject {
    Q_OBJECT

public:
    explicit Hl2TxDsp(QObject* parent = nullptr);
    ~Hl2TxDsp() override;

    struct Config {
        int inputSampleRateHz = 24000;    // AudioEngine TX audio rate
        int outputSampleRateHz = 48000;   // EP2, fixed
        int dspBlockSize = 512;           // input samples per WDSP block
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        // SSB transmit passband. Narrower than the RX default on purpose:
        // splatter outside this is other people's problem, not ours.
        double filterLowHz = 300.0;
        double filterHighHz = 2700.0;

        // Automatic level control. Speech arrives 20-30 dB below the level
        // needed to modulate fully, so without this a normal speaking voice
        // produces almost no RF — measured on hardware: audio at -10 dBFS gave
        // 1226 counts of forward power, at -30 dBFS gave 47, and speech sits
        // around -32 dBFS. A real transceiver closes that gap with mic gain,
        // compression and ALC; this is the ALC.
        bool alcEnabled = true;
        double alcTargetPeak = 0.85;   // leave headroom below clipping
        double alcMaxGainDb = 40.0;    // do not amplify a silent room forever
        double alcAttackSec = 0.005;   // catch a syllable's onset
        double alcReleaseSec = 0.500;  // slow enough not to pump between words

        // Below this input peak the ALC HOLDS its gain instead of continuing to
        // raise it. This is what stops the stage behaving like a second
        // compressor once the operator has an explicit one.
        //
        // Without it, every pause between words is a signal to keep increasing
        // gain — up to alcMaxGainDb, which is 40 dB — so room noise, mic hiss
        // and the shack fan are lifted to the same target peak as speech, and
        // the next syllable arrives into a stage that has to attack 40 dB back
        // down. That is audible as pumping, and it gets worse, not better, when
        // the operator enables the speech processor: the compressor raises the
        // average level, the ALC re-levels it away, and the two chase each
        // other. Holding through pauses leaves the ALC doing the one job it is
        // needed for — makeup gain for a quiet mic, without which speech at
        // around -32 dBFS barely modulates — and stops it re-deciding that job
        // during silence.
        //
        // NOT a gate: nothing is muted, and gain REDUCTION is never held off
        // (see processAudioBlock), because an ALC that cannot pull down on a
        // transient is a splatter generator. This only suppresses the *upward*
        // move while the input is too quiet to be speech.
        double alcHoldBelowDbfs = -45.0;
    };

    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    // Linear gain applied to the audio before modulation. 1.0 = unity.
    Q_INVOKABLE void setMicGain(double linear);
    [[nodiscard]] double micGain() const noexcept { return m_micGain; }
    // Gain the ALC is currently applying, in dB. 0 means unity.
    [[nodiscard]] double alcGainDb() const noexcept;

public slots:
    // Mono TX audio at inputSampleRateHz.
    //
    // `clientLeveled` marks audio whose level is owned by an external client —
    // TCI or DAX TX audio (WSJT-X, fldigi, the PipeWire bridge), where the
    // sender has already applied its own power/attenuation control.
    //
    // THE CONTRACT IS ONE-SIDED, and its two halves are different claims:
    //
    //   * The CLIENT owns its level upward. Nothing here adds gain it did not
    //     ask for — the ALC's makeup half is ceilinged at unity for such
    //     blocks. That half is #4796: an ALC exists to close the 20-30 dB gap
    //     between a microphone and full modulation, and applied to a client
    //     that sets its own level it does the opposite of what either party
    //     wants — it normalizes the client's level control away above the hold
    //     threshold, and freezes into a path-dependent gain below it.
    //   * The MODULATOR owns its own ceiling. Reduction still applies, because
    //     that half was never the bug. m_micGain reaches 10x (+20 dB), so a
    //     full-scale client with the TX gain slider up arrives well inside the
    //     hard clamp in processAudioBlock, and flat-topping an SSB modulator
    //     input splatters across the band. That clamp is a backstop, not a
    //     level control, and must not become the only thing standing between a
    //     hot client and the air.
    //
    // The hold (alcHoldBelowDbfs) belongs to the makeup half and so applies to
    // the mic path only. Leaving it on this path would strand a client-leveled
    // over at whatever reduction its loudest block called for — #4796
    // mirrored. hl2_txdsp_test pins all three of these claims.
    //
    // The engine's own generated audio (WSPR beacon, AX.25 modem tones, the
    // RADE modem waveform) arrives with this false and keeps the whole ALC,
    // matching its on-air level to date.
    void processAudioBlock(const std::vector<float>& mono, bool clientLeveled);
    // Drop anything buffered — on unkey, so the next transmission does not
    // start with the tail of the previous one.
    void reset();

signals:
    void iqReady(const std::vector<std::complex<float>>& iq);   // at outputSampleRateHz
    void micPeak(float dbfs);                                   // post-gain, pre-modulation
    void alcGain(float db);                                     // ALC gain applied
    // Post-ALC, post-limit peak in dBFS — the level actually handed to the
    // modulator. A LEVEL, not a gain: this is what an ALC meter shows, and it
    // moves opposite to alcGain (the harder the ALC works on a quiet mic, the
    // closer to full scale this sits).
    void alcPeak(float dbfs);
    // Echoed back from setMicGain, so a readout can report the gain THIS OBJECT
    // holds rather than the caller's copy of what it asked for.
    //
    // That distinction is the whole reason this signal exists. Mic gain was
    // dead on this backend for a release because the slider's Flex verb was
    // dropped and nothing bridged it here — and every readback available at the
    // time reported the requesting side, so all of them agreed the control
    // worked. A confirmation sourced from the requester cannot detect a request
    // that never arrived.
    void micGainChanged(double linear);

private:
    void designFilters();
    bool isLowerSideband() const;

    // Filter length. 255 taps at 48 kHz gives a transition sharp enough for a
    // 300 Hz low edge and, with a Blackman window, opposite-sideband
    // suppression well past what the transmitter needs.
    static constexpr std::size_t kTaps = 255;

    Config m_config;
    double m_micGain = 1.0;
    int m_upsample = 2;
    double m_alcGain = 1.0;      // current ALC gain, carried across blocks

    std::vector<float> m_bandpass;      // real bandpass
    std::vector<float> m_hilbert;       // quadrature half of the analytic bandpass
    std::vector<float> m_hist;          // shared delay line
    std::size_t m_histPos = 0;

    std::vector<float> m_inBuffer;      // pending input audio
    std::vector<std::complex<float>> m_iq;
};

}  // namespace AetherSDR::hl2
