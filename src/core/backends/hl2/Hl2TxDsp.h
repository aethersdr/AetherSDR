#pragma once

#include "core/backends/hl2/Hl2TxAlc.h"
#include "core/dsp/WdspChannel.h"
#include "core/backends/hl2/Hl2TxLevelPolicy.h"
#include "core/TxCoordinator.h"

#include <QElapsedTimer>
#include <QObject>

#include <complex>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "core/backends/TxAudioSource.h"

class QTimer;

namespace AetherSDR::hl2 {

// Transmit chain for the Hermes-Lite 2: processed TX audio in (AudioEngine
// has already applied tone, compressor, EQ), baseband IQ out for EP2. 24 kHz
// in, fixed 48 kHz out; WDSP's three-rate channel interpolates. SSB and digital
// in both builds; AM, DSB and FM in the TXA build only (the phasing modulator
// is SSB-only, and Hl2Backend declares those modes receive-only there).
//
// The modulator is chosen only at build time by AETHER_HL2_TX_TXA (no runtime
// switch): 1 (default) = WDSP TXA channel; 0 = in-tree phasing modulator, not
// built by any CI job (see CMakeLists.txt). The level chain (mic gain, ALC,
// clamp, meters) is shared by both.
//
// Handedness differs between them:
//   * Phasing emits the standard analytic signal and must be CONJUGATED for
//     the HPSDR wire, or every signal goes out on the wrong sideband.
//   * TXA must NOT be conjugated: fir_bandpass builds exp(-j*w_osc*pos), so it
//     already has the wire's handedness. The passband SIGN carries the
//     sideband, since TXASetupBPFilters treats TXA_LSB and TXA_USB identically.
// Config::filterLowHz/filterHighHz stay positive audio-domain magnitudes; the
// TXA build applies the sign in applyModeAndFilter(), from txaPassband(). Keep it
// there: Hl2Backend's table feeds the readback and stored eSSB pair, which want
// magnitudes.
// The TXA build depends on caller cadence; see processAudioBlock.
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
        // TXA's AM carrier level and FM peak deviation (Hl2TxLevelPolicy.h).
        // Inert outside AM/DSB and FM, and in the phasing build. No operator
        // control reaches either.
        double amCarrierLevel = kTxAmCarrierLevel;
        double fmDeviationHz = kTxFmDeviationHz;

        // ALC, reduction only: unity ceiling, never adds gain (as WDSP's
        // create_txa `alc`, max_gain=1.0; makeup gain is the separate leveler).
        // Speech at ~-32 dBFS is closed by the operator's mic gain
        // (Hl2TxLevelPolicy.h, +40 dB), not here. Fast attack, slow release, so
        // over-level input is limited rather than flat-topped by the clamp.
        bool alcEnabled = true;
        double alcTargetPeak = 0.85;   // leave headroom below clipping
        // No attack constant: each sample that needs the reduction has it
        // (Hl2TxAlc.h), so nothing overshoots into the hard clamp.
        double alcReleaseSec = 0.500;  // slow enough not to pump between words
    };

    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);
    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    // Linear gain applied to the audio before modulation. 1.0 = unity.
    Q_INVOKABLE void setMicGain(double linear);
    [[nodiscard]] double micGain() const noexcept { return m_micGain; }

    // Last applied configuration; check isConfigured() before publishing it.
    // In the phasing build this struct is the modulator's whole state. In the
    // TXA build it is what the channel was asked for; the channel's own
    // readbacks are wdspChannelId() and modulatorFaultBlocks().
    [[nodiscard]] const Config& config() const noexcept { return m_config; }
    [[nodiscard]] bool isConfigured() const noexcept { return m_configured; }
    // Read-back validity belongs to the session, unlike reset() on normal unkey.
    // Called on the DSP's I/O thread; does not change the signal-processing state.
    void invalidateConfiguration() noexcept { m_configured = false; }
    // Gain the ALC is currently applying, in dB. 0 means unity.
    [[nodiscard]] double alcGainDb() const noexcept;

    // Which modulator this binary was built with, for the health snapshot.
    [[nodiscard]] static const char* modulatorName() noexcept;

    // Signed TXA passband for `mode` from the positive audio pair: [+lo, +hi]
    // for USB/DIGU/CWU and unlisted modes, [-hi, -lo] for LSB/DIGL/CWL, and
    // [-hi, +hi] for AM/SAM/DSB/FM. The symmetric band is an audio low-pass:
    // bp0 runs before xammod/xfmmod, which read only I, so a one-sided band
    // would halve the modulation. Pure; only the TXA build calls it.
    [[nodiscard]] static std::pair<double, double>
    txaPassband(WdspChannel::Mode mode, double lowHz, double highHz) noexcept;

    // The WDSP-allocated channel id, or -1 when this build has none.
    [[nodiscard]] int wdspChannelId() const noexcept;
    [[nodiscard]] const WdspChannel::Config* channelConfig() const noexcept;

    // Blocks the modulator could not place on the wire since configure(): each
    // non-Ok WdspChannel::processIq in the TXA build, always 0 in the phasing
    // build. Surfaced in the log and health snapshot. Read on the I/O thread,
    // where processAudioBlock also runs, so no atomic.
    [[nodiscard]] unsigned long long modulatorFaultBlocks() const noexcept;
    [[nodiscard]] unsigned long long modulatorBlocks() const noexcept;
    // The part of modulatorFaultBlocks() dropped unexchanged because the worker
    // had not produced the previous block's output within a block period (TXA
    // build; 0 in the phasing build). A drop means the worker was starved; any
    // other fault means the exchange gate failed.
    [[nodiscard]] unsigned long long modulatorStallDrops() const noexcept;

    // Test seam: filters the readiness answer the exchange gate acts on. It can
    // only narrow the channel's own answer (make the worker look stalled),
    // never exchange into a channel that is not ready. Empty = unfiltered.
    // TXA build only; a no-op in the phasing build.
    void setOutputReadyProbeForTest(std::function<bool(bool channelReady)> probe);

public slots:
    // Mono TX audio at inputSampleRateHz. `source` decides whether m_micGain
    // applies:
    //   Microphone / ClientLeveled  yes: the mic level, and a proportional
    //                               attenuator on TCI/DAX audio (#4796). The
    //                               AX.25 modem (fixed 0.35, -9.12 dBFS) is
    //                               Microphone. RADE never reaches here.
    //   EngineGenerated             no: the WSPR pump keys unattended and the
    //                               mic slider must not move or mute it.
    // The ALC applies to all. m_inBuffer carries up to dspBlockSize-1 samples
    // between calls; a source change mid-transmission drops that carry.
    //
    // TXA build: the channel is non-blocking, and one underrun leaves its output
    // ring out of step for the rest of the over (fexchange2 advances r2_outidx
    // anyway). So each exchange waits for the channel's output
    // (exchangeDueBlocks()); a caller faster than real time on average grows a
    // warned queue instead. Failed blocks count in modulatorFaultBlocks().
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
    // Echoed from setMicGain so a readout reports the gain this object holds,
    // not the caller's copy of the request.
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

    Config m_config;
    TxCoordinator::Context m_txContext;
    bool m_configured = false;
    double m_micGain = 1.0;
    // The source of the last block processed, so carried m_inBuffer residue is
    // never levelled as a different source. See processAudioBlock().
    TxAudioSource m_lastSource = TxAudioSource::Microphone;
    bool m_sourceChangeWarned = false;   // one warning per transmission
    int m_upsample = 2;
    Hl2TxAlc m_alc;              // level chain; its gain carries across blocks

    std::vector<float> m_inBuffer;      // pending input audio
    // Levelled audio for one call: the hand-off point between the shared level
    // chain and whichever modulator is compiled in. Sized on demand, reused
    // across calls so the real-time path does not allocate per block.
    std::vector<float> m_levelled;
    std::vector<std::complex<float>> m_iq;

#if AETHER_HL2_TX_TXA
    std::unique_ptr<WdspChannel> m_channel;
    // Whether the TXA channel is started; reset() stops it and the next block
    // restarts it. Tracked because setRunning() is a control call.
    bool m_modulatorRunning = false;
    // Permanent zero Q plane: audio is mono, and xpanel's inselect = 2
    // (create_txa) zeroes Q anyway (measured).
    std::vector<float> m_zeroQ;
    std::vector<float> m_outI;
    std::vector<float> m_outQ;
    unsigned long long m_txBlocks = 0;
    unsigned long long m_txFaultBlocks = 0;

    // Exchange pacing. One capture delivery can carry several blocks (two per
    // delivery at 24 kHz), and a back-to-back exchange underruns. Levelled
    // audio waits here; a block is exchanged only when outputReady() says the
    // previous block's output is out, and a 1 ms timer retries the rest. A head
    // block still not ready after exchangeStallNs() is dropped, not exchanged.
    void exchangeDueBlocks();
    void onExchangeTimer();
    void noteFault(const char* why);
    bool channelOutputReady();
    qint64 exchangeStallNs() const noexcept;
    std::vector<float> m_exchangePending;
    QTimer* m_exchangeTimer = nullptr;
    // When the block now at the head of m_exchangePending got there. Invalid
    // while no whole block waits and after every over boundary, so an over
    // never inherits a deadline from the previous one.
    QElapsedTimer m_headWait;
    unsigned long long m_txStallDrops = 0;
    // Queue depth, in blocks, above which the stage warns that it is being fed
    // faster than the channel drains. Warned once per excursion.
    static constexpr std::size_t kPendingWarnBlocks = 4;
    bool m_pendingWarned = false;
    std::function<bool(bool)> m_outputReadyProbe;
#else
    // 255 taps at 48 kHz with a Blackman window: enough opposite-sideband
    // suppression for voice at a 300 Hz low edge, not at the digital modes'
    // 150 Hz (measured 22.06 dB on {150, 3000}), which is why TXA is the default.
    static constexpr std::size_t kTaps = 255;

    // The phasing modulator's only reading of the mode: LSB, CWL and DIGL.
    bool isLowerSideband() const;

    std::vector<float> m_bandpass;      // real bandpass
    std::vector<float> m_hilbert;       // quadrature half of the analytic bandpass
    std::vector<float> m_hist;          // shared delay line
    std::size_t m_histPos = 0;
#endif
};

}  // namespace AetherSDR::hl2
