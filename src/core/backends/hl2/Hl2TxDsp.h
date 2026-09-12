#pragma once

#include "core/dsp/WdspChannel.h"

#include <QObject>

#include <complex>
#include <string>
#include <vector>
#include "core/backends/IRadioBackend.h"

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

        // Automatic level control, PROTECTION ONLY: it may reduce gain, never
        // add it. The ceiling is unity and there is no field that can raise it.
        //
        // That is what the name has always meant elsewhere. create_txa() in
        // third_party/wdsp/upstream/TXA.c builds the stage it calls `alc` with
        // run=1 and max_gain=1.0 — always on, and structurally incapable of
        // adding gain — and puts the gain that CAN be added in a separate
        // `leveler`, built run=0 (off by default) with max_gain=1.778, which is
        // +5 dB. Two stages, two jobs, and only one of them is an ALC.
        //
        // This stage was previously both, with makeup up to 40 dB and an
        // absolute hold threshold below which it stopped lifting. The measured
        // consequence was that 20.5 dB of speech-to-floor separation went in
        // and 0.33 dB came out: the hold sat below the room, so the room was
        // lifted level with the speech. The gap it was closing is real —
        // measured on hardware, audio at -10 dBFS gave 1226 counts of forward
        // power, at -30 dBFS gave 47, and speech sits around -32 dBFS — but a
        // level-dependent makeup stage is the wrong instrument for it. The
        // operator's mic gain closes it instead, which is why
        // Hl2TxLevelPolicy.h's slider now reaches +40 dB rather than +20.
        //
        // What is left here is the half that was never the bug: reduction, on a
        // fast attack and a slow release, so an over-level input is limited
        // smoothly rather than flat-topped by the hard clamp behind it. An ALC
        // that cannot pull down on a transient is a splatter generator.
        bool alcEnabled = true;
        double alcTargetPeak = 0.85;   // leave headroom below clipping
        double alcAttackSec = 0.005;   // catch a syllable's onset
        double alcReleaseSec = 0.500;  // slow enough not to pump between words
    };

    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    // Linear gain applied to the audio before modulation. 1.0 = unity.
    Q_INVOKABLE void setMicGain(double linear);
    [[nodiscard]] double micGain() const noexcept { return m_micGain; }

    // Last applied configuration. Read-back consumers must check isConfigured()
    // before publishing it: defaults/refused or abandoned setups are not live.
    //
    // Unlike the receive side there is no WDSP channel behind this, so there is
    // no lower level to query: Hl2TxDsp is a hand-written phasing modulator and
    // this struct IS its state. A read of it is therefore level 4 in the
    // read-back sense, not a weaker stand-in for one.
    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] bool isConfigured() const noexcept { return m_configured; }
    // Read-back validity belongs to the session, unlike reset() on normal unkey.
    // Called on the DSP's I/O thread; does not change the signal-processing state.
    void invalidateConfiguration() noexcept { m_configured = false; }
    // Gain the ALC is currently applying, in dB. 0 means unity.
    [[nodiscard]] double alcGainDb() const noexcept;

public slots:
    // Mono TX audio at inputSampleRateHz.
    //
    // `clientLeveled` marks audio whose level is owned by an external client —
    // TCI or DAX TX audio (WSJT-X, fldigi, the PipeWire bridge), where the
    // sender has already applied its own power/attenuation control.
    //
    // THE FLAG IS NOW INERT HERE, and saying so is the point of this comment.
    //
    // It used to select the ALC's ceiling: unity for client-leveled audio,
    // alcMaxGainDb (40 dB) for everything else. That asymmetry was #4796 — an
    // ALC applied to a client that sets its own level normalized that level
    // control away above the hold threshold and froze into a path-dependent
    // gain below it. The remedy was to ceiling the client path at unity.
    //
    // The ceiling is now unity on EVERY path, so the two branches have
    // converged and there is nothing left for the flag to select. What
    // survives is the half that was never the bug and never depended on the
    // flag: the MODULATOR owns its own ceiling. Reduction still applies to
    // everything, because m_micGain reaches 100x (+40 dB, Hl2TxLevelPolicy.h)
    // and a full-scale source with the TX gain slider up arrives far inside
    // the hard clamp below — and flat-topping an SSB modulator input splatters
    // across the band. That clamp is a backstop, not a level control, and must
    // not become the only thing standing between a hot source and the air.
    //
    // THE PARAMETER HAS A JOB AGAIN, AND IT IS THE MIC SLIDER.
    //
    // It went vestigial when the ceiling became unity — it had selected the
    // ceiling and qualified the hold, and both went. It is now a THREE-STATE
    // source (TxAudioSource), and what it decides is whether m_micGain applies
    // at all:
    //
    //   Microphone / ClientLeveled   m_micGain applies. On the mic path it is
    //                                the operator's own level control; on the
    //                                client path it is a proportional
    //                                attenuator, which is what #4796 left it.
    //   EngineGenerated              m_micGain DOES NOT APPLY. A mic slider is
    //                                a microphone control. The WSPR pump, the
    //                                AX.25 modem and the RADE waveform each
    //                                generate at a level chosen for them, and
    //                                yoking a beacon to the setting an operator
    //                                picked for their voice is a defect that
    //                                predates the ALC change — it was merely
    //                                invisible while 40 dB of makeup
    //                                normalised every one of them to target.
    //
    // The ALC itself is unchanged for all three: reduction-only, unity ceiling.
    // Engine audio is protected from splatter exactly like everything else; it
    // simply is not RE-LEVELLED on its way in.
    //
    // hl2_txdsp_test's #4796 cases still pass unchanged, which is the evidence
    // that none of this moved the TCI/DAX path.
    void processAudioBlock(const std::vector<float>& mono,
                           TxAudioSource source);
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
    bool m_configured = false;
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
