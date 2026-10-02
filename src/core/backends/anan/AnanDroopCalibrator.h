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
    using Curve = std::array<float, anan::kDroopCorrectionFftSize>;

    // Per-bin MEDIAN across N dB captures (robust to a stray in-band signal in one
    // capture on a live antenna). Computed in linear power and converted back: a
    // no-op for a median, but correct if the reducer ever becomes a mean.
    [[nodiscard]] static Curve medianPowerCurve(const QVector<Curve>& captures);

    // Median of the curve over a central window (default: center +/- 15% of
    // the bins), not the exact center bin -- WDSP's analyzer does no DC
    // removal, so the centre point can carry a DC spike; the median over the
    // window ignores it.
    [[nodiscard]] static float referenceLevel(const Curve& curve,
                                              float windowFraction = 0.15f);

    // correction[k] = clamp(referenceDb - curve[k], 0, capDb). Measured into a
    // dummy load, so `curve` is the noise floor and droop is a deterministic CIC/
    // decimation attenuation applied equally to signal and noise; restoring it is not
    // amplifying noise. Bench edges at 1536 ksps sit 15-20 dB below a 70 dB-capped
    // correction, so the cap is 90 dB; it only bounds a garbage measurement.
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

    // Write half of loadTables(); the float<->JSON codec and validity rules live
    // here only. MERGES into this radio's existing tables, since a partial sweep's
    // Apply carries only the rates it measured (see stop()). Refuses a stored schema
    // newer than this build. Returns "" on success, else the operator-facing reason.
    // Sole caller: AnanBackend::invokeExtension()'s applyDroopTables().
    [[nodiscard]] static QString saveTables(
        const RadioSettingsScope& scope,
        const QMap<int, anan::DroopCorrectionTable>& tables);

public slots:
    // Begins a 6-rate sweep from Idle. No-op if already running or the
    // radio has no active panadapter yet.
    void start();

    // Aborts a running sweep: stops the poll timer (nothing else is awaited), lifts
    // the DSP bypass, best-effort restores the pre-sweep rate (restoreRate=false on
    // disconnect/destruction) and returns to Idle. Rates already measured are KEPT;
    // hasResult()/applyResult() work with a partial set.
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
    // The analyzer reseeds its average from the first frame at a new rate
    // (AnanPanAnalyzer), so this only covers the first frames landing: 500 ms is a
    // dozen frames at 25 fps.
    static constexpr int kPostLandSettleMs = 500;
    static constexpr int kPollIntervalMs = 200;
    // Bound on Sampling without frames (hidden/paused panadapter, short frames,
    // network drop); otherwise isRunning() latches, the correction stays bypassed
    // and the radio stays at the sweep rate. Generous for low spectrum FPS.
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
