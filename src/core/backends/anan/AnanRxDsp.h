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

// The ANAN-G2 receive DSP stage: turns raw DDC0 IQ blocks (from
// P2Client::ddc0IqReady) into demodulated audio (WdspChannel), a panadapter
// spectrum (AnanPanAnalyzer, WDSP's display analyzer), and an uncalibrated
// raw signal-peak reading kept
// below the seam until a measured dBFS-to-dBm calibration exists. The eventual
// AnanBackend owns one and runs it on the backend's I/O thread. Mirrors
// Hl2RxDsp's shape closely -- same WdspChannel, same "owns a DSP chain"
// branch of the seam already proven there -- but scoped to exactly what
// the one-DDC, RX-only ANAN path needs. The existing WDSP impulse blanker
// runs ahead of demodulation; manual notch filters are not implemented here.
//
// *** READ HERMES.md §16 ("Receive handedness and tuning — the two-error
// trap") BEFORE TOUCHING processIqBlock(). *** It documents the most
// expensive bug in this project's history: Hl2RxDsp handed the demodulator
// and the spectrum the WRONG IQ handedness for a full bring-up cycle. This
// class's conjugate split mirrors Hl2RxDsp's STRUCTURE (proven: WDSP's own
// passband-sign convention transfers unchanged) -- and, as of 2026-08-21,
// its polarity is CONFIRMED for Protocol 2 too, not just assumed: both
// halves of the "two-source bar" HERMES §16 sets are in. `radiocert rx`
// (2026-08-19, real WWV carrier) showed the textbook USB/DIGU-recover,
// LSB/DIGL-don't signature. Then, independently, an RSP1B running SDR++ --
// sharing zero code with this backend -- reproduced the exact same
// USB-hears/LSB-doesn't pattern at the same dial/offset geometry, and
// visually confirmed the panadapter draws the carrier on the correct side
// of the dial. See processIqBlock()'s handedness comment for the mechanism
// this confirms. Since the move to WDSP's analyzer the spectrum's mirror is
// done INSIDE the analyzer (Spectrum0() swaps I and Q), so both paths are fed
// the raw wire and nothing in this class conjugates; the handedness test pins
// that a wire-convention tone above centre still lands above centre.
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
        // false (live): processIq is non-blocking. true: waits for each
        // output block (deterministic for an offline/burst feed -- what the
        // handedness test uses).
        bool blockForOutput = false;
        // WDSP's impulse noise blanker (ANB), run on the raw IQ ahead of the
        // channel -- see WdspChannel::setNoiseBlanker(). In Config so a
        // rate-change rebuild opens the new channel with the operator's
        // blanker, the same way it carries mode, filter and AGC. Level is
        // 0..100 in the slice model's units.
        bool noiseBlankerEnabled = false;
        int noiseBlankerLevel = 50;
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
        // The DDC0 rate buildChannel() actually built this channel for.
        // installChannel() copies it into m_config.inputSampleRateHz on
        // swap -- the only place that field is updated for a live rate
        // change, since buildChannel() runs off this object's own thread and
        // cannot touch m_config directly. Without this, droopTableForRate()
        // keeps reading the connect-time rate forever after the first zoom,
        // applying one rate's correction curve to a different rate's data.
        int inputSampleRateHz = 0;
        std::string error;   // set iff channel == nullptr
    };

    // The slow half of what configure() does -- WdspChannel::create()'s
    // FFTW planning (up to ~a minute cold, see WdspChannel.cpp) -- split out
    // so a rate change can run it on a background thread while the
    // CURRENTLY installed channel keeps processing processIqBlock()
    // undisturbed. Static and free of `this` on purpose: the whole point is
    // that a caller can run this from anywhere without touching this
    // object's state while it's still in use elsewhere.
    [[nodiscard]] static RebuildResult buildChannel(const Config& config);

    // Seeds this object's current state before the FIRST asynchronous build.
    // installRebuiltChannel() deliberately reapplies m_config rather than the
    // background snapshot, so omitting this step would replace the requested
    // startup mode/filter/AGC with Config's member defaults. Subsequent edits
    // made while the build is running still update m_config and win at install.
    Q_INVOKABLE void beginInitialBuild(const Config& config);

    // Marks a rebuild in flight. While true, setMode()/setFilter()/setAgc()/
    // setShift() still update m_config/m_shiftHz (so operator input during
    // the build is never lost) but skip pushing to m_channel -- WDSP's
    // control calls take a process-wide setup mutex that the background
    // build already holds for the length of the FFTW planning
    // (WdspChannel.cpp's g_setupMutex), so pushing through during a build
    // would block THIS thread for however long the build has left,
    // reproducing the exact freeze this whole mechanism exists to remove.
    Q_INVOKABLE void beginRebuild();

    // Installs an already-built RebuildResult as the active channel. Must
    // run on this object's own thread (same contract as every other method
    // here), but deliberately NOT Q_INVOKABLE: RebuildResult owns two
    // unique_ptrs, and moc's generated qt_static_metacall dispatch (built
    // for EVERY Q_INVOKABLE regardless of whether it's ever actually
    // invoked that way) copy-constructs its by-value arguments out of a
    // void** array -- which a move-only type cannot satisfy, and fails to
    // compile. Every real call site reaches this directly (already running
    // on this object's own thread inside a lambda) or via
    // QMetaObject::invokeMethod's functor overload, neither of which needs
    // moc's string-dispatch machinery. Re-pushes this object's CURRENT
    // m_config/m_shiftHz -- the latest operator state, not whatever
    // buildChannel() was given, which may be stale if the operator changed
    // mode/filter/AGC/shift while the build was in flight -- to the new
    // channel, resizes scratch buffers for its outputBlockSize(), resets
    // the DC blocker, and retires the old channel/analyzer. Always clears
    // the in-flight flag set by
    // beginRebuild(). Returns false (leaving the current channel untouched)
    // if result.channel is null.
    bool installRebuiltChannel(RebuildResult result);

    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // RX frequency shift in Hz relative to the NCO -- how a single-DDC
    // backend tunes the slice inside the passband without moving the DDC.
    Q_INVOKABLE void setShift(double shiftHz);
    // Noise blanker on/off and level (0..100). Deferred like setMode() while a
    // rebuild is in flight; installChannel() re-applies it at the swap.
    Q_INVOKABLE void setNoiseBlanker(bool on, int level);
    struct NoiseBlankerState {
        bool hasChain = false;
        bool on = false;
        int level = 0;
    };
    // One atomic snapshot of the installed channel, safe to query from the
    // backend's thread. Deferred or refused requests do not change this value.
    [[nodiscard]] NoiseBlankerState noiseBlankerState() const noexcept
    {
        const int state = m_nbAppliedState.load(std::memory_order_relaxed);
        if (state < 0) {
            return {};
        }
        return {true, state >= kNbEnabledOffset, state % kNbEnabledOffset};
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

    // Installs the measured per-bin dB correction for ONE DDC0 rate (see
    // AnanDroopCorrection.h). Ignored -- no change, no crash -- if `table`
    // is not exactly kDroopCorrectionFftSize long or rateKsps is not one of
    // the six valid ANAN-G2 DDC0 rates: a caller passing a stale or
    // malformed table must never silently misalign bin k against the wrong
    // correction. Callers: AnanBackend seeds this from persisted per-radio
    // settings at connect, and AnanDroopCalibrator pushes freshly measured
    // tables live once a sweep completes. std::vector<float>, not
    // DroopCorrectionTable, because qRegisterMetaType<std::vector<float>>
    // is already registered (constructor, for spectrumReady/audioReady) --
    // reusing it avoids adding a second metatype for the same threading
    // need.
    Q_INVOKABLE void setDroopCorrectionTable(int rateKsps, const std::vector<float>& table);

    // Suspends droop correction WITHOUT discarding the measured tables, so
    // AnanDroopCalibrator can measure the radio instead of measuring its own
    // output. The sweep taps the same spectrumReady bins the panadapter
    // paints; with correction live, the second sweep an operator runs sees an
    // already-flattened curve, computes a near-zero table from it, and Apply
    // persists that over the good one.
    //
    // A bypass FLAG rather than "push kDroopCorrectionZero for each rate":
    // pushing a zero-valued table through setDroopCorrectionTable() stores a
    // COPY, so droopTableForRate() no longer returns the kDroopCorrectionZero
    // object itself and processIqBlock()'s `&droopTable != &kDroopCorrectionZero`
    // identity test still reads true -- the synthetic 12 dB edge fade would
    // stay on and be measured as if it were hardware droop. Routing the
    // bypass through droopTableForRate() keeps both suppressions on the one
    // switch they were always meant to share. It is also non-destructive: an
    // abort, a disconnect, or a crash mid-sweep cannot lose a calibration
    // that was only ever hidden, never overwritten.
    Q_INVOKABLE void setDroopCorrectionBypassed(bool bypassed);
    [[nodiscard]] bool droopCorrectionBypassed() const noexcept { return m_droopBypassed; }

    // Forgets every measured table. This object is constructed ONCE and
    // survives disconnect/reconnect, while the tables are per-RADIO -- so
    // without this, calibrated G2 #1 -> disconnect -> G2 #2 renders #2's
    // spectrum through #1's per-bin corrections (plus the edge fade on top),
    // with the Droop tab showing nothing, since it reads the calibrator's
    // measuredTables() and those are empty. AnanBackend calls this on
    // disconnect and again before seeding a fresh connect's tables.
    Q_INVOKABLE void clearDroopCorrectionTables();

    // Exposes the active channel for testing installChannel()'s reapply
    // behaviour (mode/filter/AGC/shift surviving a rebuild swap) without a
    // live radio -- matches WdspChannel's own *ForTest accessor convention.
    // Not part of the operator-facing seam; nullptr before the first
    // configure()/installRebuiltChannel().
    [[nodiscard]] const WdspChannel* channelForTest() const noexcept { return m_channel.get(); }

    // Every outcome m_channel->processIq() returned, counted, since this
    // object was constructed. IDENTICAL to Hl2RxDsp::processTally() and
    // deliberately so: this class and Hl2RxDsp collapsed all five non-`Ok`
    // results into the same unannotated `continue`, and a fix applied to one
    // copy and not the other is worse than none — a reader who finds the
    // counter on the HL2 will assume the ANAN has it too.
    //
    // `Underrun` is counted separately from the four faults because it is
    // normal while the asynchronous output side fills; see WdspProcessTally.h.
    //
    // MONOTONIC ACROSS A REBUILD, which matters more here than on the HL2:
    // this class rebuilds its channel OFF-THREAD (buildChannel /
    // installRebuiltChannel) and swaps it in under a running stream, so a
    // rebuild is the likeliest moment for a `Busy` or a geometry fault, and
    // clearing the count at exactly that moment would erase the evidence.
    //
    // Safe to call from another thread: relaxed atomics, like every other
    // cross-thread readback on this class.
    [[nodiscard]] WdspProcessTally::Counts processTally() const noexcept
    {
        return m_processTally.snapshot();
    }

    // Mute the DEMODULATOR while transmitting. Suppressing audio further
    // downstream is not enough -- this pipeline keeps demodulating our own
    // transmission and the backlog drains to the speakers on unmute. Muted,
    // the SPECTRUM still runs on real IQ; the audio channel is clocked with
    // silence instead. (TX does not exist yet in this backend -- RFC §2.11
    // Phase 3 -- but the mute path is cheap to have ready and every
    // WdspChannel RX consumer in this codebase provides one.)
    Q_INVOKABLE void setAudioMuted(bool muted);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

    // Demodulated-audio DC blocker, one pole per channel. WDSP's AM/SAM
    // detector is an envelope detector (amd.c emits sqrt(I^2+Q^2), strictly
    // non-negative), so the carrier arrives as a DC pedestal nothing
    // upstream removes -- see Hl2RxDsp::DcBlocker's header comment for the
    // full account (`levelfade` holds the pedestal rather than removing it;
    // the symmetric AM/SAM passband puts 0 Hz mid-band). A WDSP fact, not an
    // HL2 fact -- this class reuses the same WdspChannel configuration, so
    // it applies here with the same confidence.
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

    // A DDC0 sequence gap preceded the NEXT block this stage will be handed.
    // AnanBackend routes P2Client::ddcSequenceGap here by DirectConnection on
    // the I/O thread this object already lives on -- the same thread and the
    // same call chain that then delivers the block, so this is a plain call and
    // introduces no cross-thread edge.
    //
    // Drops the partly staged analyzer block that would straddle the gap, and
    // counts it (see spectrumGapDiscards()). The analyzer's own history is
    // left alone, as the openHPSDR desktop clients do on a sequence error: its
    // FFTs overlap and average over ~250 ms, so a gap costs at most a frame or
    // two of slightly blended display, and purging it would blank the
    // panadapter instead. The AUDIO path is left alone too; see
    // Hl2RxDsp::onSequenceGap().
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
    static constexpr int kNbEnabledOffset = 128; // levels occupy 0..100
    std::atomic<int> m_nbAppliedState{-1};       // -1: no installed channel
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
