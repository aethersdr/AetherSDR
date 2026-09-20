#pragma once

#include "core/dsp/WdspChannel.h"
#include "core/TxCoordinator.h"

#include <QObject>

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "core/backends/TxAudioSource.h"

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
// MODULATION is chosen AT BUILD TIME by AETHER_HL2_TX_TXA (CMakeLists.txt) and
// by nothing else. There is no runtime switch and there must not be one: two
// silent transmit paths are worse than one. What the flag selects is the
// MODULATOR only — the level chain below it (mic gain, the ALC and its
// client-leveled ceiling, the hard clamp, and all three meters) is shared, is
// identical in both builds, and is not part of the choice.
//
//   AETHER_HL2_TX_TXA=1 (default) — a WDSP TXA channel at the live geometry.
//   AETHER_HL2_TX_TXA=0 (opt-out)   — the in-tree phasing modulator, the way
//                                     back. NOT BUILT BY ANY CI JOB; see
//                                     CMakeLists.txt beside the option.
//
// THE TWO MODULATORS HAVE OPPOSITE HANDEDNESS CONVENTIONS and this is the one
// place that is easy to get catastrophically wrong, because it is invisible
// from inside this application:
//
//   * The PHASING modulator emits the standard analytic signal and must be
//     CONJUGATED for the HPSDR wire. Omitting that transmitted every signal on
//     the wrong sideband, and it took an operator with a second receiver to
//     catch it.
//   * A TXA channel must NOT be conjugated. fir_bandpass builds
//     exp(-j*w_osc*pos), so a POSITIVE signed passband already selects the
//     negative baseband half — TXA already has the wire's handedness. Instead
//     the SIGN of the passband carries the sideband, because TXASetupBPFilters
//     handles TXA_LSB and TXA_USB with the identical CalcBandpassFilter call and
//     SetTXAMode therefore does not choose a sideband for SSB at all.
//
// Config::filterLowHz/filterHighHz stay the POSITIVE, audio-domain pair
// Hl2Backend pushes for every mode, in both builds. The TXA build applies the
// sign itself, from the mode, in applyModeAndFilter(). Do not move that into
// Hl2Backend: its table is shared with the readback and with the operator's
// stored eSSB pair, and both want magnitudes.
//
// THE TXA BUILD DEPENDS ON THE CALLER'S CADENCE and the phasing build does not.
// See the note on processAudioBlock and modulatorFaultBlocks().
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
        // NO ATTACK CONSTANT. Reduction is instantaneous — see
        // processAudioBlock. A configurable attack was in this struct until it
        // was measured to be the mechanism by which the stage overshot: at a
        // 512-sample block on 24 kHz the 5 ms constant already closed 98.6% of
        // the error in one block, so it was not buying smoothing, it was
        // leaving 1.4% of a 40 dB step above the clamp.
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
    // WHAT LEVEL THIS IS depends on the build, and the difference is real.
    // In the PHASING build there is no WDSP channel behind this, so there is no
    // lower level to query: the modulator is hand-written arithmetic and this
    // struct IS its state, which makes a read of it level 4 rather than a
    // weaker stand-in for one. In the TXA build there IS a channel, this struct
    // is what it was ASKED for, and the level-4 reads are wdspChannelId() and
    // modulatorFaultBlocks(). Hl2Backend::gatherDspChains reports both, and
    // labels them, rather than letting one claim stand for two builds.
    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] bool isConfigured() const noexcept { return m_configured; }
    // Read-back validity belongs to the session, unlike reset() on normal unkey.
    // Called on the DSP's I/O thread; does not change the signal-processing state.
    void invalidateConfiguration() noexcept { m_configured = false; }
    // Gain the ALC is currently applying, in dB. 0 means unity.
    [[nodiscard]] double alcGainDb() const noexcept;

    // ── Which modulator this BINARY was built with ───────────────────────
    //
    // Reported so the health snapshot can SAY which transmit chain is running.
    // An operator cannot select the wrong one — the other one is not in the
    // binary — but they can be running the wrong BUILD, and a bug report that
    // does not say which modulator produced the signal is not actionable. This
    // is the readback that makes the build visible without making it switchable.
    [[nodiscard]] static const char* modulatorName() noexcept;

    // The WDSP channel behind the modulator, or -1 when this build has none.
    //
    // Level 4 in the read-back sense where it is not -1: it is the id WDSP
    // actually allocated, not a number this class chose.
    [[nodiscard]] int wdspChannelId() const noexcept;
    [[nodiscard]] const WdspChannel::Config* channelConfig() const noexcept;

    // Blocks the modulator could not place on the wire, since configure().
    //
    // ALWAYS 0 in the phasing build: that modulator is arithmetic and cannot
    // fail. In the TXA build it counts every non-Ok WdspChannel::processIq —
    // and it exists because THE PRIOR TXA ATTEMPT FAILED SILENTLY, which is the
    // stated reason this is a build flag at all. A transmit chain that drops
    // blocks must say so, in the log and in the health snapshot, rather than
    // leaving an operator to work it out from the other end of a QSO.
    //
    // Read on the I/O thread, which is also the thread processAudioBlock runs
    // on (Hl2Backend moves this object there), so it needs no atomic.
    [[nodiscard]] unsigned long long modulatorFaultBlocks() const noexcept;
    [[nodiscard]] unsigned long long modulatorBlocks() const noexcept;

public slots:
    // Mono TX audio at inputSampleRateHz.
    //
    // `source` SAYS WHOSE LEVEL THIS IS, and what it decides is whether
    // m_micGain applies at all:
    //
    //   Microphone / ClientLeveled   m_micGain applies. On the mic path it is
    //                                the operator's own level control; on the
    //                                client path — TCI or DAX TX audio from
    //                                WSJT-X, fldigi or the PipeWire bridge,
    //                                whose sender already applied its own
    //                                power control — it is the proportional
    //                                attenuator #4796 left it.
    //   EngineGenerated              m_micGain DOES NOT APPLY. A mic slider is
    //                                a microphone control, and the WSPR pump —
    //                                the only source tagged this way — keys for
    //                                111.6 s with nobody at the microphone.
    //                                Yoking a beacon to the setting an operator
    //                                picked for their voice is a defect that
    //                                predates the ALC change; it was merely
    //                                invisible while 40 dB of makeup normalised
    //                                every source onto the target.
    //
    // THE AX.25 MODEM IS Microphone, NOT EngineGenerated, and the reason is a
    // level rather than a label: its AFSK amplitude is a compile-time constant
    // (kTxAfskAmplitude = 0.35, -9.12 dBFS) and the packet dialog carries no
    // level control, so this slider is the only thing in the product that can
    // move a packet frame. Bypassing it would pin HF packet 7.71 dB under
    // alcTargetPeak with nothing able to raise it. (RADE never reaches here at
    // all: it needs DAX audio, activateRADE() refuses any radio that cannot
    // provide it, and a Flex modulates on its own side.)
    //
    // WHY IT IS A SOURCE AND NOT THE BOOL IT REPLACED. `clientLeveled` selected
    // the ALC's ceiling: unity for client-leveled audio, alcMaxGainDb (40 dB)
    // for everything else. That asymmetry was #4796 — an ALC applied to a
    // client that sets its own level normalized that level control away above
    // the hold threshold and froze into a path-dependent gain below it. The
    // remedy was to ceiling the client path at unity, and then the ceiling
    // became unity on EVERY path, which left the bool nothing to select. What
    // it could never say is the distinction that matters once the makeup is
    // gone: it answered "did an external client set this level?", so the
    // operator's microphone and the engine's own generators shared one bucket.
    //
    // What survives from that era is the half that was never the bug and never
    // depended on the flag: the MODULATOR owns its own ceiling. Reduction still
    // applies to everything, because m_micGain reaches 100x (+40 dB,
    // Hl2TxLevelPolicy.h) and a full-scale source with the TX gain slider up
    // arrives far inside the hard clamp below — and flat-topping an SSB
    // modulator input splatters across the band. That clamp is a backstop, not
    // a level control, and must not become the only thing standing between a
    // hot source and the air.
    //
    // The ALC itself is unchanged for all three: reduction-only, unity ceiling.
    // Engine audio is protected from splatter exactly like everything else; it
    // simply is not RE-LEVELLED on its way in.
    //
    // RESIDUE: m_inBuffer carries up to dspBlockSize-1 samples between calls and
    // would be levelled with the NEW block's multiplier, so a source change
    // inside one transmission drops the carry rather than mislevelling it. See
    // the guard at the top of processAudioBlock().
    //
    // hl2_txdsp_test's #4796 cases still pass unchanged, which is the evidence
    // that none of this moved the TCI/DAX path.
    //
    // ── THE TXA BUILD IS NOT RATE-FREE, and the phasing build is ──────────
    //
    // The phasing modulator is a convolution: N audio samples in gives exactly
    // 2N IQ samples out, whenever they are handed over and however fast.
    //
    // A TXA channel is not. It is opened with blockForOutput = false -- the
    // setting Hl2RxDsp uses and the setting every figure on #5678 was measured
    // at -- so WdspChannel::processIq RETURNS Underrun rather than waiting when
    // the channel's output side is not ready yet. A caller that feeds faster
    // than real time starves it: measured at this geometry, 240-255 of 256
    // blocks underrun unpaced, against 0 of 64 at the live 21.33 ms block
    // period and 0 at 5 ms. An underrun is not a dropped block either --
    // fexchange2 advances r2_outidx on the miss without consuming, so the
    // stream thereafter runs one whole DSP buffer AHEAD, permanently.
    //
    // The live caller is paced: AudioEngine's TX poll hands this stage audio as
    // the sound card produces it. Every OTHER caller -- a test, a bench
    // harness, an offline render -- has to pace itself or it is measuring
    // starvation. It will not be told quietly: a starved block is counted in
    // modulatorFaultBlocks() and logged.
    //
    // THIS IS ORTHOGONAL TO THE SOURCE ARGUMENT ABOVE. The rate coupling is a
    // property of which MODULATOR was compiled in; the source argument is about
    // which LEVEL policy applies. Neither reads the other.
    void processAudioBlock(const std::vector<float>& mono,
                           TxAudioSource source,
                           const TxCoordinator::Context& context);
    // Drop anything buffered — on unkey, so the next transmission does not
    // start with the tail of the previous one.
    void reset();

signals:
    void iqReady(const std::vector<std::complex<float>>& iq,
                  const AetherSDR::TxCoordinator::Context& context); // at outputSampleRateHz
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
    // Build (or rebuild) whatever modulator this build compiled in. Both
    // implementations are total: on a false return nothing is configured.
    bool buildModulator(std::string* error);
    // Push m_config's mode and passband at the modulator. A no-op in the
    // phasing build, where the mode is read per block and the passband change
    // is designFilters(); the real work in the TXA build, where BOTH have to
    // reach the channel and the sideband rides on the passband's sign.
    void applyModeAndFilter();
    // The one modulation step. Takes LEVELLED audio at inputSampleRateHz --
    // post mic gain, post ALC, post clamp -- and appends wire-order IQ at
    // outputSampleRateHz to m_iq. Everything above it is shared between builds.
    void modulate(std::span<const float> audio);
    void resetModulatorState();
    bool isLowerSideband() const;

    Config m_config;
    TxCoordinator::Context m_txContext;
    bool m_configured = false;
    double m_micGain = 1.0;
    // The source of the last block processed, so carried m_inBuffer residue is
    // never levelled as a different source. See processAudioBlock().
    TxAudioSource m_lastSource = TxAudioSource::Microphone;
    bool m_sourceChangeWarned = false;   // one warning per transmission
    int m_upsample = 2;
    double m_alcGain = 1.0;      // current ALC gain, carried across blocks

    std::vector<float> m_inBuffer;      // pending input audio
    // Levelled audio for one call: the hand-off point between the shared level
    // chain and whichever modulator is compiled in. Sized on demand, reused
    // across calls so the real-time path does not allocate per block.
    std::vector<float> m_levelled;
    std::vector<std::complex<float>> m_iq;

#if AETHER_HL2_TX_TXA
    // ── WDSP TXA ──────────────────────────────────────────────
    std::unique_ptr<WdspChannel> m_channel;
    // Whether the TXA channel is started. reset() discards its buffered data
    // and stops it; the next over's first block starts it again.
    // Tracked rather than queried because setRunning() is [[nodiscard]] and a
    // redundant start on every block would be a control call per 21 ms.
    bool m_modulatorRunning = false;
    // A permanently zero Q plane. The backend feeds MONO audio, and xpanel runs
    // with inselect = 2 (create_panel's ninth argument in create_txa) so a TXA
    // channel multiplies Q by zero regardless -- measured, not read: a tone fed
    // in Q alone produces exact zeros on every block, forever, with no error
    // and no underrun. Handing it zeros is therefore the honest arrangement
    // rather than a waste, and it is what the sweep measured.
    std::vector<float> m_zeroQ;
    std::vector<float> m_outI;
    std::vector<float> m_outQ;
    unsigned long long m_txBlocks = 0;
    unsigned long long m_txFaultBlocks = 0;
#else
    // ── Phasing modulator ────────────────────────────────────
    //
    // Filter length. 255 taps at 48 kHz gives a transition sharp enough for a
    // 300 Hz low edge and, with a Blackman window, opposite-sideband
    // suppression well past what a VOICE transmitter needs -- and NOT enough at
    // the 150 Hz low edge the digital modes use, which is the whole reason the
    // opt-in build uses TXA. Measured: 22.06 dB at 150 Hz on {150, 3000}.
    static constexpr std::size_t kTaps = 255;

    std::vector<float> m_bandpass;      // real bandpass
    std::vector<float> m_hilbert;       // quadrature half of the analytic bandpass
    std::vector<float> m_hist;          // shared delay line
    std::size_t m_histPos = 0;
#endif
};

}  // namespace AetherSDR::hl2
