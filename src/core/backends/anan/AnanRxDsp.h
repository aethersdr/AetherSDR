#pragma once

#include <QElapsedTimer>
#include <QObject>

#include <cmath>
#include <complex>
#include <memory>
#include <numbers>
#include <vector>

#include "core/backends/anan/AnanSpectrum.h"
#include "core/dsp/WdspChannel.h"

namespace AetherSDR::anan {

// The ANAN-G2 receive DSP stage: turns raw DDC0 IQ blocks (from
// P2Client::ddc0IqReady) into demodulated audio (WdspChannel), a panadapter
// spectrum (AnanSpectrum), and an uncalibrated raw signal-peak reading kept
// below the seam until a measured dBFS-to-dBm calibration exists. The eventual
// AnanBackend owns one and runs it on the backend's I/O thread. Mirrors
// Hl2RxDsp's shape closely -- same WdspChannel, same "owns a DSP chain"
// branch of the seam already proven there -- but scoped to exactly what
// aetherd ANAN P2 Phase 1b needs (see 02-working-plan.md Step 2): no noise
// blanker, no manual notch filters. Both are real HL2 features and neither
// is speculative to add later; they are simply not part of this phase's
// one-DDC, RX-only scope, matching Hl2RxDsp's OWN history -- its notch/NB
// machinery arrived in later commits, not its first one.
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
// of the dial. See the comment at the one point processIqBlock()
// conjugates for the mechanism this confirms.
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
        int fftSize = 1024;              // panadapter FFT size
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
    };

    // Synchronous convenience used by deterministic tests. Production first
    // connect and rate changes both use the split build/install path below so
    // FFTW planning never blocks this object's I/O thread.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);

    // Result of building a WdspChannel + AnanSpectrum pair OFF this object's
    // own thread -- reads and writes nothing on `this`, so it is safe to
    // call from any thread. Move-only (owns two unique_ptrs).
    struct RebuildResult {
        std::unique_ptr<WdspChannel> channel;
        std::unique_ptr<AnanSpectrum> spectrum;
        std::size_t outputBlockSize = 0;
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
    // the DC blocker and spectrum-smoothing state, and retires the old
    // channel/spectrum. Always clears the in-flight flag set by
    // beginRebuild(). Returns false (leaving the current channel untouched)
    // if result.channel is null.
    bool installRebuiltChannel(RebuildResult result);

    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // RX frequency shift in Hz relative to the NCO -- how a single-DDC
    // backend tunes the slice inside the passband without moving the DDC.
    Q_INVOKABLE void setShift(double shiftHz);
    // Cap how often a panadapter frame is produced, in frames per second.
    // See Hl2RxDsp::setSpectrumRateFps's comment for why this is done HERE
    // (skipping the FFT entirely when a frame is not due) rather than by
    // throttling downstream -- the reasoning is WDSP/FFT-cost arithmetic,
    // not anything ANAN-specific, and transfers unchanged.
    Q_INVOKABLE void setSpectrumRateFps(int fps);

    // Exposes the active channel for testing installChannel()'s reapply
    // behaviour (mode/filter/AGC/shift surviving a rebuild swap) without a
    // live radio -- matches WdspChannel's own *ForTest accessor convention.
    // Not part of the operator-facing seam; nullptr before the first
    // configure()/installRebuiltChannel().
    [[nodiscard]] const WdspChannel* channelForTest() const noexcept { return m_channel.get(); }

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

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady per
    // FFT frame and audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

signals:
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    void spectrumReady(const std::vector<float>& binsDbfs); // DC-centred dBFS
    // WDSP's own signal-strength meter (SignalPeak), NOT the RMS of the
    // demodulated audio -- the AGC holds audio level roughly constant, so an
    // audio-RMS meter would barely move with signal strength.
    void meterUpdate(float dbfs);

private:
    bool spectrumFrameDue();

    // Shared install step for a successful RebuildResult -- resizes scratch
    // buffers, resets the DC blocker and spectrum smoothing, re-applies
    // m_config/m_shiftHz's CURRENT values to the new channel (not whatever
    // it was built with, which may be stale), and takes ownership of
    // result's channel/spectrum. Used by configure() and the asynchronous
    // first-connect/rate-change path so their final installation cannot drift.
    void installChannel(RebuildResult result);

    // Per-bin exponential smoothing applied to EVERY emitted spectrumReady
    // frame -- not just a display nicety, but the fix for a real bench
    // symptom: this radio's waterfall row comes from a SINGLE raw FFT
    // snapshot (AnanSpectrum::process() is one un-averaged periodogram, no
    // multi-frame power averaging -- see its own header), paced by
    // RadioModel's separate waterfall-rate control
    // (panFeedWaterfallRowReady()), which reads bins straight from this
    // signal with no smoothing of its own. SpectrumWidget's OWN client-side
    // EMA (m_smoothed, SMOOTH_ALPHA) softens the TRACE but is fed by the
    // SAME raw snapshot for the waterfall's primary row path, so it does not
    // help there. Doing it here, once, before emission, smooths both. See
    // kSpectrumSmoothAlpha's own comment for why the value is lighter than
    // the client-side one.
    void smoothSpectrumBins(std::vector<float>& binsDbfs);
    std::vector<float> m_smoothedBins;   // persists across frames; see smoothSpectrumBins()
    // Weight on the NEW frame each call (1 - this on the running average).
    // Lighter than SpectrumWidget's client-side SMOOTH_ALPHA (0.35): the
    // trace already gets THAT smoothing on top of this one, so a second,
    // equally-heavy pass would compound into a noticeably laggier trace, not
    // just a smoother waterfall. This value alone is enough to visibly
    // reduce single-snapshot periodogram noise without adding much lag.
    static constexpr float kSpectrumSmoothAlpha = 0.5f;

    std::unique_ptr<WdspChannel> m_channel;
    std::unique_ptr<AnanSpectrum> m_spectrum;
    double m_shiftHz = 0.0;
    Config m_config;
    // See beginRebuild()/installRebuiltChannel()'s own comments.
    bool m_rebuildInFlight = false;

    bool m_audioMuted = false;
    int m_spectrumIntervalMs = 0;   // 0 = uncapped
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;

    std::vector<std::complex<float>> m_iqBuffer;    // IQ awaiting a full DSP block
    // Wire IQ conjugated for the SPECTRUM only -- see processIqBlock().
    std::vector<std::complex<float>> m_conjugated;
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    DcBlocker m_dcBlockL, m_dcBlockR;
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch
};

}  // namespace AetherSDR::anan
