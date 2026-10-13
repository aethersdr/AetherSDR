#pragma once

#include "core/PcmFrame.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <vector>

#include "core/backends/anan/AnanDroopCorrection.h"
#include "core/backends/anan/P2Protocol.h"   // kDdc0RatesKsps
#include "core/backends/anan/AnanPanAnalyzer.h"
#include "core/dsp/WdspChannel.h"
#include "core/dsp/WdspProcessTally.h"

#include <QMap>

namespace AetherSDR::anan {

// The ANAN-G2 receive DSP stage: raw DDC0 IQ blocks (P2Client::ddc0IqReady) in;
// demodulated audio (WdspChannel), panadapter spectrum (AnanPanAnalyzer) and an
// uncalibrated raw signal-peak reading (no dBFS-to-dBm calibration yet) out.
// Runs on AnanBackend's I/O thread; same shape as Hl2RxDsp, scoped to the
// RX-only path. The WDSP impulse blanker runs ahead of demodulation; manual
// notches are not implemented.
// READ docs/HERMES.md §16 (receive handedness) BEFORE TOUCHING processIqBlock().
// P2 polarity is confirmed by `radiocert rx` (WWV: USB/DIGU recover, LSB/DIGL
// don't) and independently by SDR++ on an RSP1B. Both demodulator and analyzer
// take the raw wire IQ (the analyzer's Spectrum0() swaps I/Q itself); nothing
// here conjugates, and the handedness test pins a tone above centre landing
// above centre.
class AnanRxDsp : public QObject {
    Q_OBJECT

public:
    explicit AnanRxDsp(QObject* parent = nullptr);
    ~AnanRxDsp() override;

    // WDSP's internal DSP rate. Constant regardless of the DDC0 IQ rate or
    // the audio rate -- see the note in configure() (mirrors Hl2RxDsp's
    // kWdspDspSampleRateHz exactly; this is a WDSP fact, not a radio fact).
    static constexpr int kWdspDspSampleRateHz = 48000;

    struct Config {
        int inputSampleRateHz = 48000;   // DDC0 IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's
        // native RX rate; every valid DDC0 rate (48/96/192/384/768/1536 kHz)
        // divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        // Panadapter output points per frame. The FFT behind them is larger
        // (AnanPanAnalyzer: at least 16384) and averaged down to this count.
        // Follows the panel's width (setPanPoints()); the droop tables stay
        // at kDroopCorrectionFftSize and are read onto this count
        // (applyDroopCorrectionDbResampled()).
        int panPoints = 1024;
        // Display frame rate the analyzer is sized for; kept current by
        // setSpectrumRateFps() so a rebuild comes up at the operator's rate.
        int spectrumFps = 25;
        // Panadapter time-average, ms; 0 = none. Kept current by
        // setSpectrumAverageMs() for the same reason.
        int spectrumAverageMs = 0;
        // Log-recursive (true) or linear-recursive (false) averaging; kept
        // current by setSpectrumLogAverage().
        bool spectrumLogAverage = true;
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // RX AGC. WdspChannel's own defaults (mode 3 / 120 dB ceiling) run
        // the radio wide open -- 120 dB is the TOP of WDSP's AGC range, not
        // a sane operating ceiling. Matches Hl2RxDsp::Config's measured-safe
        // default; AnanBackend (commit 4) owns the operator-facing mapping.
        int agcMode = 3;
        double maximumAgcGainDb = 39.0;
        // The gain with AGC off (WDSP's fixed gain). WdspChannel's default;
        // AnanBackend supplies the operator's.
        double agcFixedGainDb = 10.0;
        // false (live): processIq is non-blocking. true: waits for each
        // output block (deterministic for an offline/burst feed -- what the
        // handedness test uses).
        bool blockForOutput = false;
        // WDSP's impulse noise blankers (ANB and NOB), run on the raw IQ ahead
        // of the channel -- see WdspChannel::setNoiseBlanker(). In Config so a
        // rate-change rebuild opens the new channel with the operator's
        // blanker, the same way it carries mode, filter and AGC. Level is
        // 0..100 in the slice model's units; the fill applies to Advanced.
        WdspChannel::NoiseBlanker noiseBlanker = WdspChannel::NoiseBlanker::Off;
        int noiseBlankerLevel = 50;
        WdspChannel::NoiseBlankerFill noiseBlankerFill =
            WdspChannel::NoiseBlankerFill::Zero;
    };

    // Synchronous convenience used by deterministic tests. Production first
    // connect and rate changes both use the split build/install path below so
    // FFTW planning never blocks this object's I/O thread.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);

    // Result of building a WdspChannel + AnanPanAnalyzer pair OFF this
    // object's own thread -- reads and writes nothing on `this`, so it is safe
    // to call from any thread. Move-only (owns two unique_ptrs).
    //
    // `analyzer` is declared AFTER `channel` so it is destroyed FIRST: it uses
    // the channel's id as its analyzer slot, and must be gone before the
    // channel's destructor returns that id to the pool.
    struct RebuildResult {
        std::unique_ptr<WdspChannel> channel;
        std::unique_ptr<AnanPanAnalyzer> analyzer;
        std::size_t outputBlockSize = 0;
        // The DDC0 rate this channel was built for. installChannel() copies it into
        // m_config.inputSampleRateHz -- the only update path for a live rate change,
        // since buildChannel() cannot touch m_config -- so droopTableForRate() selects
        // the right rate's table.
        int inputSampleRateHz = 0;
        std::string error;   // set iff channel == nullptr
    };

    // The slow half of configure() (WdspChannel::create()'s FFTW planning, up to ~a
    // minute cold), static and `this`-free so a rate change runs it on a background
    // thread while the installed channel keeps processing.
    [[nodiscard]] static RebuildResult buildChannel(const Config& config);

    // Seeds this object's current state before the FIRST asynchronous build.
    // installRebuiltChannel() deliberately reapplies m_config rather than the
    // background snapshot, so omitting this step would replace the requested
    // startup mode/filter/AGC with Config's member defaults. Subsequent edits
    // made while the build is running still update m_config and win at install.
    Q_INVOKABLE void beginInitialBuild(const Config& config);

    // Marks a rebuild in flight: setMode()/setFilter()/setAgc()/setAgcFixedGain()/
    // setShift() still update m_config/m_shiftHz but skip m_channel, because WDSP
    // control calls take the setup mutex (WdspChannel.cpp's g_setupMutex) the
    // background build holds for its FFTW planning -- pushing would block this thread.
    Q_INVOKABLE void beginRebuild();

    // Installs a built RebuildResult on this object's thread. Not Q_INVOKABLE: moc's
    // dispatch copy-constructs by-value arguments, which the move-only RebuildResult
    // cannot satisfy; callers invoke directly or via invokeMethod's functor overload.
    // Re-pushes the CURRENT m_config/m_shiftHz (newer than the build's snapshot),
    // resizes scratch buffers, resets the DC blocker, retires the old
    // channel/analyzer and clears the in-flight flag. Returns false, leaving the
    // current channel untouched, if result.channel is null.
    bool installRebuiltChannel(RebuildResult result);

    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // The gain with AGC off, in dB. Deferred like setAgc() while a rebuild is
    // in flight; installChannel() re-applies it at the swap.
    Q_INVOKABLE void setAgcFixedGain(double fixedGainDb);
    // RX frequency shift in Hz relative to the NCO -- how a single-DDC
    // backend tunes the slice inside the passband without moving the DDC.
    Q_INVOKABLE void setShift(double shiftHz);
    // Which blanker, its level (0..100) and Advanced's fill. Deferred like
    // setMode() while a rebuild is in flight; installChannel() re-applies it at
    // the swap.
    Q_INVOKABLE void setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                     WdspChannel::NoiseBlankerFill fill);
    struct NoiseBlankerState {
        bool hasChain = false;
        WdspChannel::NoiseBlanker kind = WdspChannel::NoiseBlanker::Off;
        int level = 0;
        WdspChannel::NoiseBlankerFill fill = WdspChannel::NoiseBlankerFill::Zero;
        [[nodiscard]] bool on() const noexcept
        {
            return kind != WdspChannel::NoiseBlanker::Off;
        }
    };
    // One atomic snapshot of the installed channel, safe to query from the
    // backend's thread. Deferred or refused requests do not change this value.
    [[nodiscard]] NoiseBlankerState noiseBlankerState() const noexcept
    {
        const int state = m_nbAppliedState.load(std::memory_order_relaxed);
        if (state < 0) {
            return {};
        }
        return {true,
                static_cast<WdspChannel::NoiseBlanker>(
                    (state / kNbLevelStride) % kNbKindStride),
                state % kNbLevelStride,
                static_cast<WdspChannel::NoiseBlankerFill>(
                    state / (kNbLevelStride * kNbKindStride))};
    }
    // Cap how often a panadapter frame is produced, in frames per second.
    // Also re-sizes the analyzer's overlap and averaging weights for the new
    // rate, so one FFT still completes per display frame.
    Q_INVOKABLE void setSpectrumRateFps(int fps);
    // Panadapter time-average, ms; 0 = none. See
    // AnanPanAnalyzer::setAverageTimeMs().
    Q_INVOKABLE void setSpectrumAverageMs(int ms);
    // Log-recursive (true) or linear-recursive (false) time averaging. See
    // AnanPanAnalyzer::setLogAverage().
    Q_INVOKABLE void setSpectrumLogAverage(bool on);
    // Panadapter output points per frame, from the panel's width. Clamped to
    // AnanPanAnalyzer::kMaxPoints; below 2 is ignored. See
    // AnanPanAnalyzer::setNumPoints().
    Q_INVOKABLE void setPanPoints(int points);

    // Installs the per-bin dB correction for ONE DDC0 rate (AnanDroopCorrection.h).
    // Ignored if `table` is not kDroopCorrectionFftSize long or the rate is not a
    // valid DDC0 rate, so bins never misalign. std::vector<float> reuses an already
    // registered metatype.
    Q_INVOKABLE void setDroopCorrectionTable(int rateKsps, const std::vector<float>& table);

    // Suspends droop correction WITHOUT discarding tables, so the calibrator measures
    // the radio rather than its own corrected output. A flag, not zero tables: a
    // stored zero table is a copy, so processIqBlock()'s identity test against
    // kDroopCorrectionZero would keep the 12 dB edge fade on and measure it as
    // droop. Routed through droopTableForRate(), and non-destructive on abort/crash.
    Q_INVOKABLE void setDroopCorrectionBypassed(bool bypassed);
    [[nodiscard]] bool droopCorrectionBypassed() const noexcept { return m_droopBypassed; }

    // Forgets every measured table. This object outlives disconnect while tables
    // are per-RADIO, so AnanBackend calls this on disconnect and before seeding a
    // new connect; otherwise radio #1's corrections render radio #2.
    Q_INVOKABLE void clearDroopCorrectionTables();

    // Exposes the active channel for testing installChannel()'s reapply
    // behaviour (mode/filter/AGC/shift surviving a rebuild swap) without a
    // live radio -- matches WdspChannel's own *ForTest accessor convention.
    // Not part of the operator-facing seam; nullptr before the first
    // configure()/installRebuiltChannel().
    [[nodiscard]] const WdspChannel* channelForTest() const noexcept { return m_channel.get(); }

    // Every m_channel->processIq() outcome since construction; identical to
    // Hl2RxDsp::processTally(). `Underrun` is counted apart from the four faults as
    // it is normal while output fills (WdspProcessTally.h). MONOTONIC ACROSS
    // REBUILDS: the off-thread swap is the likeliest moment for a `Busy` or
    // geometry fault. Relaxed atomics; safe from any thread.
    [[nodiscard]] WdspProcessTally::Counts processTally() const noexcept
    {
        return m_processTally.snapshot();
    }

    // Mute the DEMODULATOR while transmitting: muting downstream would let the
    // backlog of our own transmission drain on unmute. The spectrum still runs on
    // real IQ; audio is clocked with silence. (No TX yet, RFC §2.11 Phase 3.)
    Q_INVOKABLE void setAudioMuted(bool muted);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

    // Demodulated-audio DC blocker, one pole per channel. WDSP's AM/SAM detector is
    // an envelope detector (amd.c: sqrt(I^2+Q^2)), so the carrier is a DC pedestal
    // nothing upstream removes; see Hl2RxDsp::DcBlocker.
    struct DcBlocker {
        float r = 0.0f;    // pole radius, set by configure(); <= 0 bypasses
        float x1 = 0.0f;
        float y1 = 0.0f;

        [[nodiscard]] float process(float x) noexcept
        {
            if (!(r > 0.0f))
                return x;   // bypass when unconfigured -- r=0 would be a differentiator
            float y = x - x1 + r * y1;
            if (!(std::fabs(y) > 1e-20f))
                y = 0.0f;   // flush denormals (slow on x86) during silence
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept { x1 = 0.0f; y1 = 0.0f; }
    };

    // -3 dB corner, Hz. Matches Hl2RxDsp::kDcBlockerCornerHz -- well below
    // even a wide AM passband's audio content.
    static constexpr double kDcBlockerCornerHz = 20.0;

    [[nodiscard]] static float dcBlockerPole(double cornerHz,
                                             double sampleRateHz) noexcept
    {
        return static_cast<float>(
            std::exp(-2.0 * std::numbers::pi * cornerHz / sampleRateHz));
    }

    // ── Panadapter integrity across a transport gap ───────────────────────
    //
    // Partial FFT windows discarded at DDC0 discontinuities, including
    // accepted rewinds and duplicates. Same lifetime/empty-window semantics
    // as Hl2RxDsp::spectrumGapDiscards(). AnanBackend does not expose health
    // rows yet; this counter is currently available only at the DSP stage.
    [[nodiscard]] quint64 spectrumGapDiscards() const noexcept
    {
        return m_spectrumGapDiscards.load(std::memory_order_relaxed);
    }

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady when
    // a display frame is due and the analyzer has a new one, and
    // audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

    // A DDC0 sequence gap precedes the NEXT block (DirectConnection on this I/O
    // thread, same call chain). Drops and counts the staged analyzer block that would
    // straddle the gap (spectrumGapDiscards()); the analyzer history and audio path
    // are left alone, as the openHPSDR clients do (~250 ms averaging makes a gap
    // cost a frame or two; purging would blank the panadapter).
    void onSequenceGap();

signals:
    void pcmReady(const AetherSDR::PcmFrame& frame);
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    // WDSP's own signal-strength meter (SignalAverage), NOT the RMS of the
    // demodulated audio -- the AGC holds audio level roughly constant, so an
    // audio-RMS meter would barely move with signal strength.
    void meterUpdate(float dbfs);

private:
    void publishNoiseBlankerState();
    // The applied blanker in ONE int, so the backend's thread reads a coherent
    // snapshot from a single relaxed load rather than three that can tear:
    // level + stride * kind + stride * kinds * fill. Strides are powers of two
    // and larger than the ranges they carry (levels 0..100, three kinds, five
    // fills), so each field stays legible in a debugger.
    static constexpr int kNbLevelStride = 128; // levels occupy 0..100
    static constexpr int kNbKindStride = 8;    // three kinds today
    std::atomic<int> m_nbAppliedState{-1};     // -1: no installed channel
    PcmProducer m_pcmProducer;
    bool spectrumFrameDue();

    // Shared install step for a successful RebuildResult -- resizes scratch
    // buffers, resets the DC blocker, re-applies
    // m_config/m_shiftHz's CURRENT values to the new channel (not whatever
    // it was built with, which may be stale), and takes ownership of
    // result's channel/analyzer. Used by configure() and the asynchronous
    // first-connect/rate-change path so their final installation cannot drift.
    void installChannel(RebuildResult result);

    // See spectrumGapDiscards(). Written on the I/O thread by onSequenceGap(),
    // read by whatever polls it; relaxed for the same reasons Hl2RxDsp gives.
    std::atomic<quint64> m_spectrumGapDiscards {0};
    std::unique_ptr<WdspChannel> m_channel;
    // Declared after m_channel so it is destroyed first -- see RebuildResult.
    std::unique_ptr<AnanPanAnalyzer> m_analyzer;
    double m_shiftHz = 0.0;
    Config m_config;
    // See beginRebuild()/installRebuiltChannel()'s own comments.
    bool m_rebuildInFlight = false;

    // Per-outcome counters for m_channel->processIq(); see processTally().
    // Written on the DSP thread in processIqBlock(), read from elsewhere.
    WdspProcessTally m_processTally;

    bool m_audioMuted = false;
    // The S-meter tap's gate: the settle window after our own silence or a
    // channel install, and the read cadence -- see WdspSMeter.h, and its use
    // in processIqBlock(). DSP thread only, like m_audioMuted.
    WdspSMeterTap m_meterTap;
    // Arms it from the CURRENT config. Both call sites run after
    // m_config.inputSampleRateHz has taken the new rate.
    void armMeterSettle() noexcept
    {
        m_meterTap.arm(m_config.inputSampleRateHz, m_config.dspBlockSize,
                       kWdspDspSampleRateHz);
    }
    int m_spectrumIntervalMs = 0;   // 0 = uncapped
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;

    std::vector<std::complex<float>> m_iqBuffer;    // IQ awaiting a full DSP block
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    DcBlocker m_dcBlockL, m_dcBlockR;
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch

    // Live droop-correction tables, keyed by DDC0 rate in ksps -- see
    // setDroopCorrectionTable(). Survives a rate-change rebuild untouched:
    // installRebuiltChannel() swaps m_channel/m_analyzer, not this object,
    // and the correction is applied to m_bins after the analyzer, independent of
    // which WdspChannel produced the IQ that fed it.
    QMap<int, DroopCorrectionTable> m_droopTables;
    // See setDroopCorrectionBypassed(). Deliberately NOT cleared by
    // clearDroopCorrectionTables(): "am I mid-sweep" is a property of the
    // sweep, not of which tables happen to be loaded.
    bool m_droopBypassed = false;
    [[nodiscard]] const DroopCorrectionTable& droopTableForRate(int rateKsps) const noexcept;
};

}  // namespace AetherSDR::anan
