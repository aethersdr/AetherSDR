#include "TxLinearityController.h"

#include "TxSpectrumAnalyzer.h"
#include "core/LogManager.h"

#include <QStringList>
#include <QTimer>

namespace AetherSDR::txlin {
namespace {

// Interlock evaluation cadence. Fast enough that the hard timeout is honoured
// to within a tick, slow enough to be free. Deliberately independent of the
// telemetry arrival rate — see the header.
constexpr int kTickMs = 100;

}  // namespace

// ── TxAnalysisWorker ────────────────────────────────────────────────────────

TxAnalysisWorker::TxAnalysisWorker(int fftSize, WindowKind window, ImdConfig imd,
                                   int targetAverages)
    : m_imd(imd), m_targetAverages(targetAverages > 0 ? targetAverages : 1), m_fftSize(fftSize)
{
    m_analyzer = std::make_unique<TxSpectrumAnalyzer>(fftSize, window);
}

TxAnalysisWorker::~TxAnalysisWorker() = default;

void TxAnalysisWorker::submit(CaptureFrame&& frame)
{
    // A refused push is a dropped frame, counted by the ring. Dropping is
    // harmless for a Welch average and vastly preferable to blocking the
    // thread that is servicing the radio's stream.
    m_ring.push(std::move(frame));
}

void TxAnalysisWorker::resetAverages()
{
    m_analyzer->reset();
    m_clippedSinceReset = false;
    m_sampleRateHz = 0.0;
    // The ring may still hold frames from the previous run. Draining them here
    // would fold pre-reset samples into the new average.
    CaptureFrame stale;
    while (m_ring.pop(stale)) {
    }
}

void TxAnalysisWorker::drain()
{
    CaptureFrame frame;
    bool any = false;
    while (m_ring.pop(frame)) {
        if (int(frame.feedback.size()) != m_fftSize) {
            continue;
        }
        // A rate change mid-run invalidates the accumulated average: the bins
        // no longer mean the same frequencies.
        if (m_sampleRateHz != 0.0 && frame.sampleRateHz != m_sampleRateHz) {
            m_analyzer->reset();
            m_clippedSinceReset = false;
        }
        m_sampleRateHz = frame.sampleRateHz;
        m_clippedSinceReset = m_clippedSinceReset || frame.clipped;
        if (m_analyzer->accumulate(frame.feedback)) {
            any = true;
        }
    }
    if (!any) {
        return;
    }

    emit progress(m_analyzer->averages(), m_targetAverages);
    if (m_analyzer->averages() < m_targetAverages) {
        return;
    }

    const LinearityResult result =
        measureTwoTone(*m_analyzer, m_sampleRateHz, m_imd, m_clippedSinceReset);
    emit resultReady(result);
}

// ── TxLinearityController ───────────────────────────────────────────────────

TxLinearityController::TxLinearityController(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<AetherSDR::txlin::LinearityResult>();
    qRegisterMetaType<AetherSDR::txlin::AbortReason>();
    m_clock.start();
    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);
    connect(m_tick, &QTimer::timeout, this, &TxLinearityController::tick);
}

TxLinearityController::~TxLinearityController()
{
    // The last line of defence. If anything above unwound past stopRun(), the
    // radio still stops transmitting here rather than staying keyed because an
    // object went away.
    releaseKey();
    teardown();
}

qint64 TxLinearityController::nowMs() const
{
    return m_clock.elapsed();
}

void TxLinearityController::setCaptureSource(ITxCaptureSource* source)
{
    if (m_running) {
        stopRun(AbortReason::OperatorStop);
    }
    m_source = source;
}

PreflightResult TxLinearityController::preflight(const RunRequest& request) const
{
    RunRequest req = request;
    req.captureReady = req.captureReady && (m_source != nullptr);
    return m_safety.preflight(nowMs(), req);
}

bool TxLinearityController::startRun(const RunRequest& request, const RunConfig& config)
{
    // Preflight is run HERE, not merely offered to the caller, so there is no
    // code path that transmits without it.
    const PreflightResult pre = preflight(request);
    if (!pre.ok()) {
        QStringList warnings;
        for (const std::string& w : pre.warnings) {
            warnings << QString::fromStdString(w);
        }
        emit refused(QString::fromStdString(pre.message), warnings);
        return false;
    }
    if (m_source == nullptr || m_keyRequest == nullptr) {
        emit refused(QStringLiteral("The analyzer is not wired to a radio"), {});
        return false;
    }

    m_config = config;

    // Analysis thread first: if it cannot be brought up, nothing has been keyed
    // and there is nothing to unwind.
    m_worker = new TxAnalysisWorker(config.capture.frameSamples, config.window,
                                    config.imd, config.targetAverages);
    m_worker->moveToThread(&m_analysisThread);
    connect(&m_analysisThread, &QThread::finished, m_worker, &QObject::deleteLater);
    // The run ends the moment it has the averages it asked for. Publishing the
    // result and then continuing to transmit would be a longer emission than
    // the measurement needs, which §8 has no interest in permitting.
    connect(m_worker, &TxAnalysisWorker::resultReady, this,
            [this](const LinearityResult& result) {
                emit resultReady(result);
                stopRun(AbortReason::None);
            });
    connect(m_worker, &TxAnalysisWorker::progress, this, &TxLinearityController::progress);
    m_analysisThread.start();

    m_source->setFrameCallback([this](const CaptureFrame& frame) {
        // Runs on the thread delivering IQ. Copy into the ring and return: no
        // DSP, no allocation beyond the frame's own buffer, no signal emission.
        if (m_worker != nullptr) {
            CaptureFrame copy = frame;
            m_worker->submit(std::move(copy));
        }
    });
    m_source->setErrorCallback([this](const std::string& reason) {
        qCWarning(lcTransmit) << "TxLinearityController: capture error —"
                              << QString::fromStdString(reason);
        // Queued so the abort runs on the controller's thread even when the
        // capture path reports from another one.
        QMetaObject::invokeMethod(this, [this] { stopRun(AbortReason::CaptureLost); },
                                  Qt::QueuedConnection);
    });

    if (!m_source->start(config.capture)) {
        teardown();
        emit refused(QStringLiteral("The TX capture stream could not be started"), {});
        return false;
    }

    // Key LAST, once every non-transmitting prerequisite has succeeded. A
    // failure after this point unkeys; a failure before it never keyed.
    if (!m_keyRequest(true)) {
        teardown();
        emit refused(QStringLiteral("The radio refused the transmit request"), {});
        return false;
    }
    m_keyed = true;
    m_keyedAtMs = nowMs();
    m_gateOpened = false;
    m_running = true;
    m_safety.noteRunStarted(m_keyedAtMs);
    m_tick->start();

    qCInfo(lcTransmit) << "TxLinearityController: run started, timeout"
                       << m_safety.limits().maxTransmitMs << "ms, target averages"
                       << config.targetAverages;
    emit runStateChanged(true);
    return true;
}

void TxLinearityController::stopRun(AbortReason reason)
{
    if (!m_running && !m_keyed) {
        return;
    }
    // UNKEY FIRST. Everything below this line is bookkeeping and none of it is
    // allowed to sit between the decision to stop and the radio actually
    // stopping.
    releaseKey();

    const bool wasRunning = m_running;
    m_running = false;
    m_tick->stop();
    m_safety.noteRunStopped(nowMs());
    teardown();

    if (wasRunning) {
        qCInfo(lcTransmit) << "TxLinearityController: run stopped —"
                           << QString::fromStdString(abortReasonText(reason));
        emit runStateChanged(false);
        // None is a completed measurement and OperatorStop is a deliberate
        // one; neither is an abort the operator needs told about.
        if (reason != AbortReason::None && reason != AbortReason::OperatorStop) {
            emit aborted(reason, QString::fromStdString(abortReasonText(reason)));
        }
    }
}

void TxLinearityController::releaseKey()
{
    if (!m_keyed) {
        return;
    }
    m_keyed = false;
    if (m_keyRequest != nullptr) {
        m_keyRequest(false);
    }
}

void TxLinearityController::teardown()
{
    if (m_source != nullptr) {
        // Clear the callbacks before stopping so a frame already in flight
        // cannot reach a worker that is about to be deleted.
        m_source->setFrameCallback(nullptr);
        m_source->setErrorCallback(nullptr);
        m_source->stop();
    }
    if (m_analysisThread.isRunning()) {
        m_analysisThread.quit();
        m_analysisThread.wait();
    }
    m_worker = nullptr;   // deleteLater'd by the thread's finished signal
    m_gateOpened = false;
}

void TxLinearityController::tick()
{
    if (!m_running) {
        return;
    }
    const qint64 now = nowMs();

    // Open the capture gate once the post-key settling guard has elapsed. PA
    // thermal and ALC settling after key-down are transients; averaging them
    // into a steady-state distortion measurement reports the transient as
    // distortion. The gate lives on the source; the controller only decides
    // when, so a source with nothing to gate ignores it.
    if (!m_gateOpened && (now - m_keyedAtMs) >= qint64(m_config.capture.settleMsAfterKey)) {
        m_gateOpened = true;
        if (m_worker != nullptr) {
            QMetaObject::invokeMethod(m_worker, "resetAverages", Qt::QueuedConnection);
        }
        m_source->setGateOpen(true);
        emit progress(0, m_config.targetAverages);
    }

    // Close it again before the hard timeout. A run that reaches its target
    // averages has already stopped by now, so this only affects a run being cut
    // short — where the last frames are taken right as the deadline arrives and
    // are the least trustworthy in the capture.
    if (m_gateOpened) {
        const qint64 remaining =
            qint64(m_safety.limits().maxTransmitMs) - (now - m_keyedAtMs);
        if (remaining <= qint64(m_config.capture.settleMsBeforeUnkey)) {
            m_source->setGateOpen(false);
        }
    }

    // Interlocks. The timeout inside evaluate() depends on nothing but the
    // clock, so it fires even with no telemetry at all.
    TelemetrySnapshot snap;
    if (m_telemetry != nullptr) {
        snap = m_telemetry();
    } else {
        // No telemetry provider wired is not permission to transmit blind: keep
        // the stamp current so only the timeout governs, and log it once at
        // start rather than aborting a run the operator explicitly asked for on
        // a radio that publishes no meters.
        snap.lastUpdateMs = now;
    }
    const AbortReason reason = m_safety.evaluate(now, snap.telemetry, snap.lastUpdateMs);
    if (reason != AbortReason::None) {
        stopRun(reason);
        return;
    }

    if (m_worker != nullptr && m_gateOpened) {
        QMetaObject::invokeMethod(m_worker, "drain", Qt::QueuedConnection);
    }
}

}  // namespace AetherSDR::txlin
