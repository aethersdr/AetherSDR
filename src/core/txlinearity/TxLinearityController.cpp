#include "TxLinearityController.h"

#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <utility>

namespace AetherSDR::txlin {
namespace {
constexpr std::int64_t kReceiveDeadlineMs = 6000;
constexpr std::int64_t kReadyLifetimeMs = 2000;
constexpr int kMaxAverages = 32;
}

TxLinearityController::TxLinearityController(std::unique_ptr<ITxCaptureSource> source,
                                             Environment environment, QObject* parent)
    : QObject(parent), m_source(std::move(source)), m_environment(std::move(environment))
{
    m_timer.setParent(this);
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, &TxLinearityController::poll);
}

TxLinearityController::~TxLinearityController()
{
    m_shuttingDown = true;
    ++m_generation;
    m_timer.stop();
    if (m_lease) { m_lease->release(); }
    if (m_source) { m_source->stop(); }
}

bool TxLinearityController::setCallbacks(ReadyCallback ready, FinishedCallback finished)
{
    if (m_state != State::Idle) { return false; }
    m_readyCallback = std::move(ready);
    m_finishedCallback = std::move(finished);
    return true;
}

bool TxLinearityController::setSafetyLimits(const SafetyLimits& limits, bool allowOverride)
{
    return m_state == State::Idle && m_safety.setLimits(limits, allowOverride);
}

void TxLinearityController::confirmDummyLoad()
{
    if (m_state == State::Idle) { m_safety.confirmDummyLoad(); }
}

bool TxLinearityController::current() const
{
    return !m_shuttingDown && QThread::currentThread() == thread()
        && m_environment.nowMs && m_environment.telemetry
        && m_environment.sessionCurrent && m_environment.sessionCurrent();
}

bool TxLinearityController::validConfig(const LinearityRunConfig& config, const RunRequest& request) const
{
    const CaptureConfig& capture = config.capture;
    const std::int64_t guards = std::int64_t(capture.settleMsAfterKey) + capture.settleMsBeforeUnkey;
    const double halfRate = capture.sampleRateHz / 2;
    return capture.frameSamples == 16384 && std::isfinite(capture.sampleRateHz)
        && capture.sampleRateHz > 0 && capture.sampleRateHz <= 192000
        && std::isfinite(capture.centerFreqHz) && capture.centerFreqHz > 0
        && capture.settleMsAfterKey >= 0 && capture.settleMsBeforeUnkey >= 0
        && guards < m_safety.limits().maxTransmitMs
        && config.targetAverages > 0 && config.targetAverages <= kMaxAverages
        && (config.window == WindowKind::BlackmanHarris7 || config.window == WindowKind::BlackmanHarris4
            || config.window == WindowKind::Hann || config.window == WindowKind::FlatTop)
        && std::isfinite(config.maxForwardWatts) && config.maxForwardWatts > 0
        && config.imd.maxOrder == request.maxOrder
        && std::isfinite(request.tone1Hz) && std::isfinite(request.tone2Hz)
        && std::abs(request.tone1Hz - capture.centerFreqHz) < halfRate
        && std::abs(request.tone2Hz - capture.centerFreqHz) < halfRate
        && std::isfinite(config.imd.noiseFloorMarginDb) && config.imd.noiseFloorMarginDb >= 0
        && std::isfinite(config.imd.toneImbalanceWarnDb) && config.imd.toneImbalanceWarnDb >= 0
        && config.imd.dcNotchBins >= 0 && config.imd.dcNotchBins < capture.frameSamples / 2
        && std::isfinite(config.imd.shoulderOffsetHz) && config.imd.shoulderOffsetHz >= 0
        && std::isfinite(config.imd.shoulderBandwidthHz) && config.imd.shoulderBandwidthHz > 0
        && std::isfinite(config.imd.acprGapHz) && config.imd.acprGapHz >= 0
        && std::isfinite(config.imd.acprBandwidthHz) && config.imd.acprBandwidthHz > 0;
}

bool TxLinearityController::prepare(const LinearityRunConfig& config, const RunRequest& request)
{
    if (m_state != State::Idle || !m_source || !current() || !m_readyCallback || !m_finishedCallback
        || !validConfig(config, request)) { return false; }
    const std::int64_t now = m_environment.nowMs();
    if (now < 0 || now > std::numeric_limits<std::int64_t>::max() - kReceiveDeadlineMs) { return false; }
    RunRequest preflightRequest = request;
    preflightRequest.captureReady = true; // Only checks configuration here; startRun requires the live check.
    m_preflight = m_safety.preflight(now, preflightRequest);
    if (!m_preflight.ok()) { m_status = m_preflight.message; return false; }
    m_config = config;
    m_request = preflightRequest;
    m_prepareMs = now;
    m_frames.clear();
    m_frames.reserve(config.targetAverages);
    m_keyObservedMs.reset();
    m_gateOpen = false;
    m_status.clear();
    m_state = State::CheckingReceive;
    const std::uint64_t generation = ++m_generation;
    const QPointer<TxLinearityController> guard(this);
    m_source->setFrameCallback([guard, generation](const CaptureFrame& frame) {
        if (guard) { guard->receiveFrame(frame, generation); }
    });
    m_source->setErrorCallback([guard, generation](const std::string& error) {
        if (guard && generation == guard->m_generation) {
            guard->finishCapture(error.empty() ? "Capture source failed" : error);
        }
    });
    m_timer.start();
    const bool started = m_source->start(config.capture);
    if (!guard || generation != m_generation) { return false; }
    if (!started) { finishCapture("Capture source refused configuration"); return false; }
    const bool checking = m_source->checkReceiveReady([guard, generation] {
        if (guard) { guard->receiveReady(generation); }
    });
    if (!guard || generation != m_generation) { return false; }
    if (!checking) { finishCapture("Receive readiness check was refused"); return false; }
    return true;
}

void TxLinearityController::receiveReady(std::uint64_t generation)
{
    if (generation != m_generation || m_state != State::CheckingReceive) { return; }
    const QPointer<TxLinearityController> guard(this);
    poll();
    if (!guard || generation != m_generation || m_state != State::CheckingReceive) { return; }
    m_readyMs = m_environment.nowMs();
    m_state = State::Ready;
    const ReadyCallback callback = m_readyCallback;
    callback();
}

bool TxLinearityController::startRun(std::shared_ptr<ITxLinearityRunLease> lease)
{
    if (m_state != State::Ready || !lease || !current()) { return false; }
    const std::int64_t now = m_environment.nowMs();
    if (now < m_readyMs || now - m_readyMs > kReadyLifetimeMs
        || now > std::numeric_limits<std::int64_t>::max() - m_safety.limits().maxTransmitMs) {
        finishCapture("Receive readiness expired"); return false;
    }
    m_preflight = m_safety.preflight(now, m_request);
    if (!m_preflight.ok()) { finishCapture(m_preflight.message); return false; }
    const TxTelemetry telemetry = m_environment.telemetry(now);
    const AbortReason refusal = m_safety.evaluateTelemetry(now, telemetry);
    if (refusal != AbortReason::None || *telemetry.forwardWatts.value > 0.5
        || lease->state() != ITxLinearityRunLease::State::AwaitingKey) {
        finishCapture(refusal != AbortReason::None ? abortReasonText(refusal) : "Radio or test operation is not idle");
        return false;
    }
    m_lease = std::move(lease);
    m_state = State::AwaitingKey;
    m_deadlineMs = now + m_safety.limits().maxTransmitMs;
    m_safety.noteRunStarted(now);
    const QPointer<TxLinearityController> guard(this);
    const std::uint64_t generation = m_generation;
    const std::shared_ptr<ITxLinearityRunLease> activeLease = m_lease;
    const bool accepted = activeLease->start(m_deadlineMs);
    if (!guard || generation != m_generation || m_state != State::AwaitingKey) { return false; }
    if (!accepted) { finishCapture("Bounded transmit operation refused start"); return false; }
    poll();
    return guard && generation == m_generation
        && (m_state == State::AwaitingKey || m_state == State::Capturing);
}

void TxLinearityController::stop()
{
    if (m_state != State::Idle && m_state != State::Releasing) { finishCapture("Stopped by the operator"); }
}

void TxLinearityController::poll()
{
    if (m_shuttingDown || m_state == State::Idle || m_state == State::Analyzing) { return; }
    if (m_state == State::Releasing) {
        if (!m_finishingCapture && m_lease && m_lease->state() == ITxLinearityRunLease::State::Released) { complete(); }
        return;
    }
    if (!current()) { finishCapture("Original radio session is no longer current"); return; }
    const std::int64_t now = m_environment.nowMs();
    if (now < m_prepareMs) { finishCapture("Measurement clock moved backwards"); return; }
    if (m_state == State::CheckingReceive) {
        if (now - m_prepareMs >= kReceiveDeadlineMs) { finishCapture("Receive readiness timed out"); }
        return;
    }
    if (m_state == State::Ready) {
        if (now < m_readyMs || now - m_readyMs > kReadyLifetimeMs) { finishCapture("Receive readiness expired"); }
        return;
    }
    const TxTelemetry telemetry = m_environment.telemetry(now);
    const AbortReason abort = m_safety.evaluate(now, telemetry);
    if (abort != AbortReason::None) { finishCapture(abortReasonText(abort)); return; }
    if (*telemetry.forwardWatts.value > m_config.maxForwardWatts) { finishCapture("Forward power exceeded the test limit"); return; }
    const ITxLinearityRunLease::State tx = m_lease->state();
    if (tx == ITxLinearityRunLease::State::Failed || tx == ITxLinearityRunLease::State::Released
        || (m_keyObservedMs && tx != ITxLinearityRunLease::State::Keyed)) {
        finishCapture("Test transmission ended before capture completed"); return;
    }
    if (now >= m_deadlineMs - m_config.capture.settleMsBeforeUnkey) {
        finishCapture("Capture window ended before enough complete frames arrived"); return;
    }
    if (tx == ITxLinearityRunLease::State::Keyed) {
        if (!m_keyObservedMs) { m_keyObservedMs = now; }
        if (!m_gateOpen && now - *m_keyObservedMs >= m_config.capture.settleMsAfterKey) {
            m_state = State::Capturing;
            m_gateOpen = true;
            m_source->setGateOpen(true);
        }
    }
}

void TxLinearityController::receiveFrame(const CaptureFrame& frame, std::uint64_t generation)
{
    if (generation != m_generation || m_state != State::Capturing || !m_gateOpen) { return; }
    const QPointer<TxLinearityController> guard(this);
    poll();
    if (!guard || generation != m_generation || m_state != State::Capturing || !m_gateOpen) { return; }
    if (frame.feedback.size() != std::size_t(m_config.capture.frameSamples)
        || frame.sampleRateHz != m_config.capture.sampleRateHz
        || !std::isfinite(frame.centerFreqHz) || std::abs(frame.centerFreqHz - m_config.capture.centerFreqHz) > 0.5
        || frame.sourceCaptureId.empty() || frame.sourceCaptureId.size() > 128 || frame.timestampNs == 0
        || (m_config.expectedCorrection && frame.correctionActive != m_config.expectedCorrection)
        || (!m_frames.empty() && (frame.timestampNs <= m_frames.back().timestampNs
            || frame.correctionActive != m_frames.front().correctionActive))
        || std::any_of(m_frames.begin(), m_frames.end(), [&](const CaptureFrame& old) { return old.sourceCaptureId == frame.sourceCaptureId; })
        || std::any_of(frame.feedback.begin(), frame.feedback.end(), [](const std::complex<float>& value) {
            return !std::isfinite(value.real()) || !std::isfinite(value.imag());
        })) {
        finishCapture("Capture metadata or samples changed during measurement"); return;
    }
    CaptureFrame stored;
    stored.feedback = frame.feedback;
    stored.sampleRateHz = frame.sampleRateHz;
    stored.centerFreqHz = frame.centerFreqHz;
    stored.timestampNs = frame.timestampNs;
    stored.sourceCaptureId = frame.sourceCaptureId;
    stored.correctionActive = frame.correctionActive;
    stored.clipped = frame.clipped;
    stored.clippingKnown = frame.clippingKnown;
    m_frames.push_back(std::move(stored));
    if (m_frames.size() == std::size_t(m_config.targetAverages)) { finishCapture({}); }
}

void TxLinearityController::finishCapture(const std::string& error)
{
    if (m_shuttingDown || m_state == State::Idle || m_state == State::Releasing) { return; }
    ++m_generation;
    m_status = error;
    m_state = State::Releasing;
    m_gateOpen = false;
    m_finishingCapture = true;
    const QPointer<TxLinearityController> guard(this);
    if (m_lease) { m_lease->release(); }
    if (!guard) { return; }
    m_source->stop();
    if (!guard) { return; }
    m_finishingCapture = false;
    if (!m_lease || m_lease->state() == ITxLinearityRunLease::State::Released) { complete(); }
}

void TxLinearityController::complete()
{
    m_timer.stop();
    const std::int64_t now = m_environment.nowMs();
    if (m_safety.running()) { m_safety.noteRunStopped(std::max(now, m_prepareMs)); }
    else { m_safety.clearDummyLoadConfirmation(); }
    m_lease.reset();
    m_state = State::Analyzing;
    LinearityRunResult result;
    result.error = m_status;
    if (result.error.empty() && now < m_prepareMs) { result.error = "Measurement clock moved backwards"; }
    if (result.error.empty() && !current()) { result.error = "Original radio session is no longer current"; }
    if (result.error.empty() && m_frames.size() == std::size_t(m_config.targetAverages)) {
        TxSpectrumAnalyzer analyzer(m_config.capture.frameSamples, m_config.window);
        bool clipped = false;
        bool clippingKnown = true;
        for (const CaptureFrame& frame : m_frames) {
            if (!analyzer.accumulate(frame.feedback)) { result.error = "Spectrum analysis rejected capture"; break; }
            clipped |= frame.clipped;
            clippingKnown &= frame.clippingKnown;
            result.captures.push_back({frame.sourceCaptureId, frame.timestampNs});
        }
        if (result.error.empty()) {
            result.correctionActive = m_frames.front().correctionActive;
            result.measurement = measureTwoTone(analyzer, m_config.capture.sampleRateHz, m_config.imd, clipped, clippingKnown);
            LinearityResult& measurement = *result.measurement;
            const double expectedLow = std::min(m_request.tone1Hz, m_request.tone2Hz) - m_config.capture.centerFreqHz;
            const double expectedHigh = std::max(m_request.tone1Hz, m_request.tone2Hz) - m_config.capture.centerFreqHz;
            if (measurement.valid && (std::abs(measurement.tone1.freqHz - expectedLow) > 2 * measurement.binHz
                || std::abs(measurement.tone2.freqHz - expectedHigh) > 2 * measurement.binHz)) {
                measurement.valid = false;
                measurement.invalidReason = "Measured tones do not match the authorized test frequencies";
            }
        }
    }
    m_frames.clear();
    m_status = result.error;
    m_state = State::Idle;
    const FinishedCallback callback = m_finishedCallback;
    callback(result);
}
} // namespace AetherSDR::txlin
