#pragma once

#include "CaptureRing.h"
#include "ITxCaptureSource.h"
#include "ImdAnalyzer.h"
#include "SpectrumWindow.h"
#include "TxLinearitySafety.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>

class QTimer;

namespace AetherSDR::txlin {

// Analysis worker. Lives on its own thread and does every transform.
//
// Separate from the controller so the FFT work is structurally incapable of
// running on the GUI thread — a 16384-point transform every few milliseconds
// would stall the panadapter, and a stalled GUI while the radio is keyed is
// exactly the condition in which an operator cannot press Stop.
class TxAnalysisWorker : public QObject {
    Q_OBJECT

public:
    TxAnalysisWorker(int fftSize, WindowKind window, ImdConfig imd, int targetAverages);
    ~TxAnalysisWorker() override;

    // Producer side, called from the capture thread. Never blocks.
    void submit(CaptureFrame&& frame);

public slots:
    // Drain whatever the ring holds and, once enough frames have averaged,
    // publish a result. Invoked via a queued connection from the controller's
    // tick so the worker owns no timer of its own.
    void drain();
    void resetAverages();

signals:
    void resultReady(const AetherSDR::txlin::LinearityResult& result);
    void progress(int averages, int target);

private:
    std::unique_ptr<class TxSpectrumAnalyzer> m_analyzer;
    ImdConfig m_imd;
    int m_targetAverages = 8;
    int m_fftSize = 0;
    double m_sampleRateHz = 0.0;
    bool m_clippedSinceReset = false;
    CaptureRing<CaptureFrame> m_ring{16};
};

// Orchestrates one linearity measurement: preflight, key, settle, capture,
// analyze, unkey.
//
// The radio-facing operations are injected as callables rather than taken as
// model pointers. That keeps the whole of `txlinearity` free of RadioModel and
// TransmitModel — the seam that lets the same controller drive an HPSDR
// backend later — and it means the run logic can be exercised in a test with
// two lambdas and no radio.
//
// TRANSMIT DISCIPLINE. Constitution Principle VI: every path fails closed.
// - stopRun() releases the key as its FIRST action, before touching capture,
//   timers, or state, and is idempotent. It is never the tail of a function
//   that can return early.
// - The destructor releases the key too, so an unwind that skips stopRun()
//   still cannot leave the radio transmitting.
// - Loss of the capture stream is an immediate abort, not a degraded mode.
// - The transmit timeout is checked on a timer that depends on nothing except
//   the clock, so a radio that has gone completely silent is still unkeyed.
class TxLinearityController : public QObject {
    Q_OBJECT

public:
    struct RunConfig {
        CaptureConfig capture;
        ImdConfig imd;
        WindowKind window = WindowKind::BlackmanHarris7;
        // Welch averages to collect before the run finishes on its own. The
        // hard transmit timeout still wins: a run that cannot reach this count
        // inside the timeout is cut short with however many it got, and the
        // result says so.
        int targetAverages = 8;
    };

    struct TelemetrySnapshot {
        TxTelemetry telemetry;
        qint64 lastUpdateMs = 0;
    };

    // Returns false if the radio refused; the controller then aborts the run
    // rather than assuming the key took.
    using KeyRequest = std::function<bool(bool key)>;
    using TelemetryProvider = std::function<TelemetrySnapshot()>;

    explicit TxLinearityController(QObject* parent = nullptr);
    ~TxLinearityController() override;

    // The controller does not own the source; the UI does, because the source
    // needs Qt signal wiring to the radio that the controller deliberately has
    // no access to.
    void setCaptureSource(ITxCaptureSource* source);
    [[nodiscard]] ITxCaptureSource* captureSource() const { return m_source; }

    void setKeyRequest(KeyRequest fn) { m_keyRequest = std::move(fn); }
    void setTelemetryProvider(TelemetryProvider fn) { m_telemetry = std::move(fn); }

    [[nodiscard]] TxLinearitySafety& safety() { return m_safety; }
    [[nodiscard]] const TxLinearitySafety& safety() const { return m_safety; }

    [[nodiscard]] PreflightResult preflight(const RunRequest& request) const;

    // Runs preflight itself and refuses if it does not pass — a caller cannot
    // skip the interlocks by not calling preflight().
    bool startRun(const RunRequest& request, const RunConfig& config);
    void stopRun(AbortReason reason = AbortReason::OperatorStop);

    [[nodiscard]] bool running() const { return m_running; }
    [[nodiscard]] qint64 nowMs() const;

signals:
    void runStateChanged(bool running);
    void resultReady(const AetherSDR::txlin::LinearityResult& result);
    void progress(int averages, int target);
    void aborted(AetherSDR::txlin::AbortReason reason, const QString& text);
    // Emitted when preflight refuses, so the UI has one place to show refusals
    // whether they came from the explicit call or from startRun().
    void refused(const QString& message, const QStringList& warnings);

private:
    void tick();
    void releaseKey();
    void teardown();

    ITxCaptureSource* m_source = nullptr;
    KeyRequest m_keyRequest;
    TelemetryProvider m_telemetry;

    TxLinearitySafety m_safety;
    RunConfig m_config;

    bool m_running = false;
    bool m_keyed = false;
    bool m_gateOpened = false;
    qint64 m_keyedAtMs = 0;

    QElapsedTimer m_clock;
    QTimer* m_tick = nullptr;
    QThread m_analysisThread;
    TxAnalysisWorker* m_worker = nullptr;
};

}  // namespace AetherSDR::txlin

Q_DECLARE_METATYPE(AetherSDR::txlin::LinearityResult)
Q_DECLARE_METATYPE(AetherSDR::txlin::AbortReason)
