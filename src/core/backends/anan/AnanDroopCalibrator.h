#pragma once

#include "core/backends/anan/AnanDroopCorrection.h"
#include "core/backends/anan/P2Protocol.h"   // kDdc0RatesKsps
#include "core/RadioSettingsScope.h"

#include <QElapsedTimer>
#include <QMap>
#include <QMetaObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <array>
#include <functional>
#include <utility>

namespace AetherSDR {

// Backend-owned sweep engine. Hooks are an injectable, socket-free boundary to
// ANAN's rate control, DSP bypass and persistence. No shared RadioModel or GUI
// object participates in the sweep; only AnanBackend feeds its spectrum/rate.
class AnanDroopCalibrator : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        std::function<bool()> available;
        std::function<void(int)> requestRate;
        std::function<void(bool)> bypass;
        std::function<QString(const QMap<int, anan::DroopCorrectionTable>&)> apply;
    };
    explicit AnanDroopCalibrator(Hooks hooks = {}, QObject* parent = nullptr);
    void setLandedRate(int rateKsps) { m_landedRateKsps = rateKsps; }
    void onSpectrumFrame(const std::vector<float>& binsDbm);

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

    // The write half, symmetric with loadTables() so the float<->JSON codec
    // and its validity rules live in ONE place rather than being hand-copied
    // between the reader and the apply handler.
    //
    // MERGES into whatever this radio already has: a partial sweep is blessed
    // as "a safe, real partial improvement" (see stop()), so an Apply after
    // one carries only the rates it measured. Writing the document from those
    // alone would drop every previously calibrated rate from disk while the
    // live DSP kept them -- the radio correct until the next connect and
    // silently wrong after it. Principle XIV: persisted as a unit.
    //
    // Refuses a row whose stored schema is NEWER than this build understands,
    // rather than merging into a shape it cannot know. Returns an empty
    // string on success, otherwise the operator-facing reason it did not
    // persist -- never void, because the caller reports that outcome to the
    // dialog and the bridge.
    //
    // Still exactly one CALLER: AnanBackend::invokeExtension()'s
    // applyDroopTables() method, never duplicated between the UI tab and
    // the bridge verb.
    [[nodiscard]] static QString saveTables(
        const RadioSettingsScope& scope,
        const QMap<int, anan::DroopCorrectionTable>& tables);

public slots:
    // Begins a 6-rate sweep from Idle. No-op if already running or the
    // radio has no active panadapter yet.
    void start();

    // Aborts a running sweep: stops accepting spectrum frames and stops the poll
    // timer (the entire cancellation -- nothing else is awaited, since a
    // rate-change confirmation is only ever polled, never blocked on),
    // lifts the DSP correction bypass, best-effort restores the pre-sweep
    // rate, and returns to Idle. Disconnect/destruction pass restoreRate=false.
    // Whatever rates were already measured before stopping are KEPT (not
    // cleared) -- a partial table set is a safe, real partial improvement,
    // not corrupt data; hasResult()/applyResult() work with it as-is.
    void stop(bool restoreRate = true);

    // Applies and persists the staged result through the owning backend.
    // Emits error(reason) on refusal and finished(true) only on success.
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

private:
    void requestCurrentRate();
    void finishSweep(bool applied, bool restoreRate = true);
    void abortSweep(const QString& reason);
    [[nodiscard]] int currentTargetRateKsps() const;

    // Bypass both correction and cosmetic edge fade while measuring the
    // backend's spectrum. The ANAN owner queues this before requesting a rate.
    void setDspBypass(bool bypassed);

    // The sweep covers every rate the radio has, in the order it has them --
    // see anan::kDdc0RatesKsps for why the set is spelled in exactly one place.
    static constexpr auto& kRatesKsps = anan::kDdc0RatesKsps;
    static constexpr int kSamplesPerRate = 8;
    static constexpr int kSampleSpacingMs = 300;
    static constexpr int kRateWaitTimeoutMs = 90'000;  // "~a minute cold" + margin
    static constexpr int kPostLandSettleMs = 500;      // EMA (kSpectrumSmoothAlpha) convergence
    static constexpr int kPollIntervalMs = 200;
    // Sampling needs its own bound. Only the rate wait used to have one, so a
    // feed that simply stopped -- a hidden or paused panadapter, a frame size
    // that is not kDroopCorrectionFftSize, a quiet network drop -- left this
    // phase spinning forever: isRunning() stayed latched (making start() and
    // applyResult() permanent no-ops), the correction stayed bypassed, and
    // the radio sat parked at the sweep's rate. Generous against a low
    // spectrum FPS while still failing in seconds rather than never.
    static constexpr int kSampleStallTimeoutMs = 15'000;

    Hooks m_hooks;
    int m_landedRateKsps = 0;
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
};

}  // namespace AetherSDR
