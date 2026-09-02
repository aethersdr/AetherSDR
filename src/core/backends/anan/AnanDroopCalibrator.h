#pragma once

#include "core/backends/anan/AnanDroopCorrection.h"
#include "core/RadioSettingsScope.h"

#include <QElapsedTimer>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <array>

namespace AetherSDR {

class RadioModel;

// Live, in-app calibration for the ANAN-G2's real DDC0 CIC/decimation droop
// (see AnanDroopCorrection.h for what the droop is and why it exists).
//
// Modeled on this codebase's own established conventions rather than
// invented fresh: the sweep-engine shape (headless QObject; steps a
// control, samples a live response, builds a curve, computes and applies a
// result; started/progress/finished signal vocabulary) mirrors
// AgcTCalibrator. The per-radio persistence shape (RadioSettingsScope
// feature document, kFeature/kSchemaVersion, a pure static load function)
// mirrors Hl2FreqCal. Family-agnostic mechanically -- it drives
// RadioModel::setPanBandwidth() and taps RadioModel::panFeedSpectrumReady
// only, both generic RadioModel surfaces -- and is exposed only for ANAN
// via RadioCapabilities::hostDroopCalibration.
//
// Owned by RadioModel (RadioModel::droopCalibrator()), not dialog-
// constructed like AgcTCalibrator: invokeBackendExtension() is a
// synchronous, fire-and-forget call correlated only by requestId (confirmed
// against Hl2Backend::invokeExtension()), which cannot deliver a
// multi-minute sweep's live progress to a UI. A persistent, directly
// reachable instance lets a UI tab and the `droopcal` bridge verb observe
// and drive the SAME sweep without fighting over or duplicating it.
class AnanDroopCalibrator : public QObject {
    Q_OBJECT

public:
    explicit AnanDroopCalibrator(RadioModel* radio, QObject* parent = nullptr);

    enum class Phase { Idle, WaitingForRateLanded, Settling, Sampling };

    [[nodiscard]] bool isRunning() const noexcept { return m_phase != Phase::Idle; }
    [[nodiscard]] int  rateIndex() const noexcept { return m_rateIdx; }
    [[nodiscard]] int  totalRates() const noexcept { return static_cast<int>(kRatesKsps.size()); }
    [[nodiscard]] bool hasResult() const { return !m_measuredTables.isEmpty(); }
    [[nodiscard]] const QMap<int, anan::DroopCorrectionTable>& measuredTables() const
    {
        return m_measuredTables;
    }

    // ---- pure math (static, unit-testable without a live radio) ----
    // Ported directly from this feature's original offline prototype
    // (a throwaway offline script, never in this tree, since superseded by
    // this in-app engine) -- same algorithm, same reasoning, now C++.
    using Curve = std::array<float, anan::kDroopCorrectionFftSize>;

    // Combines N per-capture dB curves into one, per bin, via the MEDIAN
    // across captures -- robust against a stray in-band signal landing in
    // one capture during a live-antenna sweep, unlike a mean. Converts to
    // linear power before taking the median and back to dB after: for a
    // median specifically this round trip is a no-op in exact arithmetic
    // (dB is a strictly increasing function of power, and order statistics
    // are invariant under any strictly monotonic transform), but it keeps
    // this function's contract "average in the physically meaningful
    // domain" even if a future caller swaps the reducer for a mean, where
    // the log-domain-bias problem (Jensen's inequality) is real.
    [[nodiscard]] static Curve medianPowerCurve(const QVector<Curve>& captures);

    // Median of the curve over a central window (default: center +/- 15% of
    // the bins), not the exact center bin -- avoids any residual DC-region
    // artifact even though AnanSpectrum already DC-removes before windowing.
    [[nodiscard]] static float referenceLevel(const Curve& curve,
                                              float windowFraction = 0.15f);

    // correction[k] = clamp(referenceDb - curve[k], 0, capDb). The sweep
    // measures with a dummy load, so `curve` IS the noise floor -- droop
    // here is a real, deterministic attenuation the CIC/decimation chain
    // applies equally to noise and any in-band signal, not a floor the
    // signal disappears beneath. Correcting it, even by many tens of dB, is
    // restoring the true level of whatever is actually in that bin, not
    // amplifying noise past a recoverable signal. 70 dB (the first value
    // tried, from an estimate off one bench capture) still left the worst
    // bins visibly low: with that cap applied and persisted, a live
    // panadapter capture at 1536 ksps still read edges at -15 to -20 dB
    // relative to mid-band (mid-band ~-108..-115 dBm, edges down to
    // -133..-139 dBm) -- the cap itself was the limiting factor, not the
    // correction math. 90 dB clears that with margin. capDb exists as a
    // genuine safety bound against a corrupted/garbage measurement, not as
    // a "past this point it's unrecoverable" line.
    [[nodiscard]] static anan::DroopCorrectionTable computeCorrection(
        const Curve& curve, float referenceDb, float capDb = 90.0f);

    // ---- persistence (static, no live radio needed) ----
    static constexpr const char* kFeature = "DroopCalibration";
    static constexpr int kSchemaVersion = 1;

    // Reads whatever was last persisted for this radio (possibly a partial
    // set of rates, or none). Missing/unparseable entries are simply
    // absent from the result -- AnanRxDsp's own per-rate fallback
    // (kDroopCorrectionZero) covers a rate this never measured.
    [[nodiscard]] static QMap<int, anan::DroopCorrectionTable> loadTables(
        const RadioSettingsScope& scope);

    // No saveTables() here -- the one writer is AnanBackend::invokeExtension()'s
    // "anan"/"droop.apply" handler (applyResult() below calls into it),
    // exactly once, never duplicated between a UI tab and the bridge verb.

public slots:
    // Begins a 6-rate sweep from Idle. No-op if already running or the
    // radio has no active panadapter yet.
    void start();

    // Aborts a running sweep: disconnects the spectrum tap, stops the poll
    // timer (the entire cancellation -- nothing else is awaited, since a
    // rate-change confirmation is only ever polled, never blocked on),
    // best-effort restores the pre-sweep rate, and returns to Idle.
    // Whatever rates were already measured before stopping are KEPT (not
    // cleared) -- a partial table set is a safe, real partial improvement,
    // not corrupt data; hasResult()/applyResult() work with it as-is.
    void stop();

    // Pushes every measured table live to AnanRxDsp and persists them, via
    // exactly one call into AnanBackend::invokeExtension()'s "droop.apply"
    // handler -- mirrors Hl2Backend's applyFreqCalPpb() discipline of one
    // apply path, never duplicated. No-op if hasResult() is false.
    void applyResult();

    // Drops any measured (but not yet applied) tables. No-op while running.
    void clear();

signals:
    void started();
    void progress(int rateIndex, int totalRates, int percent);
    void rateSampled(int rateKsps, int sampleCount);
    void finished(bool applied);
    void error(const QString& reason);

private slots:
    void advance();   // one poll tick of the phase state machine
    void onSpectrumFrame(quint32 streamId, const QVector<float>& binsDbm, qint64 emittedNs);

private:
    void requestCurrentRate();
    void finishSweep(bool applied);
    void abortSweep(const QString& reason);
    [[nodiscard]] int currentTargetRateKsps() const;

    static constexpr std::array<int, 6> kRatesKsps{48, 96, 192, 384, 768, 1536};
    static constexpr int kSamplesPerRate = 8;
    static constexpr int kSampleSpacingMs = 300;
    static constexpr int kRateWaitTimeoutMs = 90'000;  // "~a minute cold" + margin
    static constexpr int kPostLandSettleMs = 500;      // EMA (kSpectrumSmoothAlpha) convergence
    static constexpr int kPollIntervalMs = 200;

    RadioModel* m_radio = nullptr;   // not QPointer: RadioModel outlives this (owned member)
    QTimer m_pollTimer;
    Phase m_phase = Phase::Idle;
    int m_rateIdx = -1;
    qint64 m_phaseStartedAtMs = 0;
    qint64 m_lastSampleAtMs = 0;
    QElapsedTimer m_clock;
    int m_originalRateKsps = 0;

    bool m_haveLatestFrame = false;
    Curve m_latestFrame{};
    QVector<Curve> m_pendingCaptures;
    QMap<int, anan::DroopCorrectionTable> m_measuredTables;
    QMetaObject::Connection m_spectrumConn;
};

}  // namespace AetherSDR
