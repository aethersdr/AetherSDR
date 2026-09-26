#include "core/backends/rtl/RtlSdrWorker.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include "core/LogManager.h"
#include "core/backends/rtl/RtlSdrUsbDevice.h"
#include <aether_wdsp.h>

namespace AetherSDR::rtl {
namespace {
constexpr std::uint32_t kRtlBufLength = 16384;
using T = RtlCaptureTransaction;

} // namespace

RtlSdrWorker::RtlSdrWorker(struct rtlsdr_dev* dev, QObject* parent, std::size_t capacity)
    : RtlSdrWorker(std::make_unique<RtlSdrUsbDevice>(dev), parent, capacity)
{
}

RtlSdrWorker::RtlSdrWorker(std::unique_ptr<Device> device, QObject* parent, std::size_t capacity)
    : QThread(parent), m_device(std::move(device)), m_pipeline(std::make_unique<RtlReceivePipeline>(capacity))
{
    m_iqBuffer.resize(kRtlBufLength / 2);
    // The DDC emits synchronously inside acquisition. Stamp there, then queue
    // the worker signal to the backend's owning thread (seam contract rule 2).
    connect(&m_ddc, &RtlSdrDdc::spectrumFrameReady, this,
        [this](int panId, const QByteArray& frame) {
            emit spectrumFrameReady(m_applied.session, m_applied.revision, panId, frame);
        }, Qt::DirectConnection);
    connect(&m_ddc, &RtlSdrDdc::waterfallRowReady, this,
        [this](int panId, const QByteArray& frame) {
            emit waterfallRowReady(m_applied.session, m_applied.revision, panId, frame);
        }, Qt::DirectConnection);
    connect(&m_ddc, &RtlSdrDdc::audioFrameReady, this,
        [this](const QByteArray& pcm, const QByteArray& preMonitor) {
            emit audioFrameReady(m_applied.session, m_applied.revision, pcm, preMonitor);
        }, Qt::DirectConnection);
}

RtlSdrWorker::~RtlSdrWorker()
{
    if (!stopReading()) { qFatal("RtlSdrWorker destroyed while its USB reader is still running"); }
}

void RtlSdrWorker::startReading()
{
    if (isRunning()) { return; }
    m_stopRequested = false;
    start();
}

bool RtlSdrWorker::stopReading()
{
    m_stopRequested = true;
    for (int attempt = 0; isRunning() && attempt < 50; ++attempt) {
        if (m_device) { m_device->cancelAsync(); }
        if (wait(100)) { break; }
    }
    if (isRunning()) {
        qWarning() << "RtlSdrWorker: USB reader did not stop after repeated cancellation";
        return false;
    }
    m_pipeline->stop();
    return true;
}

bool RtlSdrWorker::submit(const Transaction::Work& work)
{
    if (work.target.receivers.empty()
        || m_command.load(std::memory_order_acquire) != Command::Idle) { return false; }
    m_work = work;
    m_result = Transaction::Result{work.token, Transaction::ResultCode::Applied, work.target, work.operation};
    if (work.hardwareChanged) {
        m_command.store(Command::Hardware, std::memory_order_release);
    } else {
        m_preparationSubmitted = false; m_prepareAttempts = 0;
        m_command.store(Command::Preparing, std::memory_order_release);
    }
    serviceCancellation();
    return true;
}

std::optional<RtlSdrWorker::Transaction::Result> RtlSdrWorker::takeResult()
{
    if (m_command.load(std::memory_order_acquire) != Command::Complete) { return {}; }
    auto result = std::move(m_result);
    m_result.reset();
    m_work.reset(); // destruction is on the backend thread, never the callback
    m_command.store(Command::Idle, std::memory_order_release);
    return result;
}

void RtlSdrWorker::serviceCancellation()
{
    const Command command = m_command.load(std::memory_order_acquire);
    if (command == Command::Idle || command == Command::Complete) {
        (void)m_pipeline->service(); // reap acknowledged banks even without another request
    }
    if (m_command.load(std::memory_order_acquire) == Command::Preparing) {
        (void)m_pipeline->service();
        if (!m_preparationSubmitted) {
            // A just-removed slot remains charged until its off-thread
            // destructor finishes. Keep one bounded request while it retires;
            // never resurrect its old handle to make reuse appear immediate.
            const auto submission = m_pipeline->prepareDetailed(m_work->target,
                !m_work->before || m_work->target.dcSuppression != m_work->before->dcSuppression,
                m_work->compensation);
            m_preparationSubmitted = submission == RtlReceivePipeline::Submission::Accepted;
            if ((submission == RtlReceivePipeline::Submission::RetryRetiringSlot
                || submission == RtlReceivePipeline::Submission::RetryPlannerBusy)
                && ++m_prepareAttempts < 100) { return; }
        }
        const auto status = m_preparationSubmitted ? m_pipeline->service() : RtlReceivePipeline::Preparation::Failed;
        if (status == RtlReceivePipeline::Preparation::Ready) {
            m_command.store(Command::Receiver, std::memory_order_release);
        } else if (status == RtlReceivePipeline::Preparation::Failed) {
            m_result = Transaction::Result{m_work->token, Transaction::ResultCode::Restored,
                m_work->before, m_work->operation};
            m_command.store(Command::Complete, std::memory_order_release);
        }
    }
    // Also called by the backend timer: retry across the narrow readAsync entry
    // race, including a device that never delivers its first callback.
    if (m_command.load(std::memory_order_acquire) == Command::Hardware
        && m_readerRunning.load(std::memory_order_acquire) && m_device) {
        m_device->cancelAsync();
    }
}

void RtlSdrWorker::applyDdc(const Transaction::State& state)
{
    m_ddc.resetSpectrum(); // no mixed-revision window, including verified rollback
    const bool resetDc = m_work->hardwareChanged || m_applied.session != state.token.session
        || m_dcSuppression != state.dcSuppression;
    if (resetDc) {
        m_dcSuppression = state.dcSuppression;
        m_dcBlocker.configure(m_dcSuppression, state.hardware.sampleRateHz);
        m_ddc.resetReceiveAudio();
    }
    const Transaction::Receiver& receiver = state.receivers.front();
    const bool legacyReceiving = m_pipeline->legacy();
    const int lowHz = static_cast<int>(receiver.passband.filterLowHz);
    const int highHz = static_cast<int>(receiver.passband.filterHighHz);
    m_ddc.applyCapture(state.hardware.sampleRateHz, state.hardware.centerHz,
                      receiver.passband.carrierHz, receiver.mode,
                      lowHz, highHz);
    if (legacyReceiving != m_legacyReceiving
        || (legacyReceiving && (lowHz != m_legacyFilterLowHz
            || highHz != m_legacyFilterHighHz))) {
        m_ddc.resetReceiveAudio();
    }
    m_legacyReceiving = legacyReceiving;
    m_legacyFilterLowHz = lowHz;
    m_legacyFilterHighHz = highHz;
    m_ddc.applyMonitor(receiver.audioGain, receiver.audioPan, receiver.audioMute);
    m_applied = state.token;
    qCDebug(lcPerf).nospace() << "RtlCapture phase=adopt ms=" << QDateTime::currentMSecsSinceEpoch()
        << " session=" << state.token.session << " revision=" << state.token.revision
        << " centerHz=" << state.hardware.centerHz << " generation=" << state.capture.generation;
}

bool RtlSdrWorker::prepareHardwareResult()
{
    if (!m_result->actual) { return false; }
    QElapsedTimer deadline; deadline.start();
    bool submitted = false;
    while (!m_stopRequested.load(std::memory_order_acquire) && deadline.elapsed() < 30000) {
        if (!submitted) {
            const auto submission = m_pipeline->prepareDetailed(*m_result->actual, true,
                m_work->compensation || m_result->code == Transaction::ResultCode::Restored);
            if (submission == RtlReceivePipeline::Submission::Failed
                || submission == RtlReceivePipeline::Submission::RetryPlannerBusy) { return false; }
            if (submission == RtlReceivePipeline::Submission::RetryRetiringSlot) {
                // Hardware is quiesced here. Reap the old bank off the callback
                // and wait briefly for its destructor before reserving a new
                // instance of the same slot. Other refusals fail immediately.
                if (deadline.elapsed() >= 2000) { return false; }
                (void)m_pipeline->service();
                QThread::msleep(1);
                continue;
            }
            submitted = true;
        }
        const auto status = m_pipeline->service();
        if (status == RtlReceivePipeline::Preparation::Ready) { return m_pipeline->adopt(); }
        if (status == RtlReceivePipeline::Preparation::Failed) { return false; }
        QThread::msleep(1); // USB is quiesced; never executed by its callback
    }
    return false;
}
void RtlSdrWorker::applyHardware()
{
    m_command.store(Command::Applying, std::memory_order_release);
    qCDebug(lcPerf).nospace() << "RtlCapture phase=hardware ms=" << QDateTime::currentMSecsSinceEpoch()
        << " session=" << m_work->token.session << " revision=" << m_work->token.revision
        << " operation=" << m_work->operation << " compensation=" << m_work->compensation;
    m_result = Transaction::execute(*m_work, *m_device);
    qCDebug(lcPerf).nospace() << "RtlCapture phase=readback ms=" << QDateTime::currentMSecsSinceEpoch()
        << " session=" << m_result->token.session << " revision=" << m_result->token.revision
        << " operation=" << m_result->operation << " result=" << int(m_result->code);
    if (!prepareHardwareResult()) {
        // Planning/admission can fail after hardware readback. Compensate the
        // complete device before reporting refusal, exactly as for a USB error.
        bool restored = false;
        if (m_work->before && !m_stopRequested.load(std::memory_order_acquire)) {
            Transaction::Work rollback = *m_work;
            rollback.target = *m_work->before; rollback.hardwareChanged = true;
            const auto result = Transaction::execute(rollback, *m_device);
            if (result.code == Transaction::ResultCode::Applied) {
                m_result = Transaction::Result{m_work->token, Transaction::ResultCode::Restored,
                    result.actual, m_work->operation};
                restored = prepareHardwareResult();
            }
        }
        if (!restored) { m_result = Transaction::Result{m_work->token, Transaction::ResultCode::Invalid, {}, m_work->operation}; }
    }
    if (m_result->actual) { applyDdc(*m_result->actual); m_firstSample = 0; }
    else { m_applied = {}; }
    m_command.store(Command::Complete, std::memory_order_release);
}

void RtlSdrWorker::run()
{
    // Materialize platform TLS outside USB callbacks (macOS may allocate its
    // thread-local backing on first access).
    (void)wdspPortThreadAllocationSequence();
    if (!m_device) { emit readError(QStringLiteral("Device handle is null")); return; }
    while (!m_stopRequested.load(std::memory_order_acquire)) {
        if (m_command.load(std::memory_order_acquire) == Command::Hardware) {
            applyHardware();
        }
        // An invalid result has no usable bank. Keep the device quiesced and
        // let the backend consume the failure and retire this worker.
        if (m_applied.revision == 0) { return; }
        if (!m_device->resetBuffer()) {
            emit readError(QStringLiteral("RTL-SDR buffer reset failed")); return;
        }
        m_readerRunning.store(true, std::memory_order_release);
        if (m_stopRequested.load(std::memory_order_acquire)) { break; }
        const int rc = m_device->readAsync(&RtlSdrWorker::rtlsdrCallback, this);
        m_readerRunning.store(false, std::memory_order_release);
        if (m_stopRequested.load(std::memory_order_acquire)) { break; }
        if (m_command.load(std::memory_order_acquire) == Command::Hardware) { continue; }
        emit readError(QStringLiteral("RTL-SDR acquisition ended unexpectedly (%1)").arg(rc));
        break;
    }
    m_readerRunning.store(false, std::memory_order_release);
}

void RtlSdrWorker::rtlsdrCallback(unsigned char* buf, std::uint32_t len, void* ctx)
{
    auto* worker = static_cast<RtlSdrWorker*>(ctx);
    if (!worker) { return; }
    const Command command = worker->m_command.load(std::memory_order_acquire);
    if (worker->m_stopRequested.load(std::memory_order_acquire) || command == Command::Hardware) {
        worker->m_device->cancelAsync();
        return;
    }
    if (!buf || len == 0 || len % 2 != 0 || len > kRtlBufLength) {
        worker->m_ddc.resetSpectrum();
        worker->m_dcBlocker.reset();
        return;
    }
    const bool adopting = command == Command::Receiver && worker->m_pipeline->adopt();
    if (adopting) { worker->applyDdc(worker->m_work->target); }
    worker->handleCallback(buf, len);
    // Complete is the release of ALL references into m_work, including DSP
    // output during this block. The control side may destroy it immediately.
    if (adopting) { worker->m_command.store(Command::Complete, std::memory_order_release); }
}

void RtlSdrWorker::handleCallback(unsigned char* buf, std::uint32_t len)
{
    if (!buf || len == 0 || len % 2 != 0 || len > kRtlBufLength) { return; }
    const std::uint32_t count = len / 2;
    // Capacity is allocated before acquisition. librtlsdr's configured maximum
    // bounds every block; resize inside that capacity constructs only floats.
    m_iqBuffer.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        m_iqBuffer[i] = {(float(buf[2 * i]) - 127.5f) / 127.5f,
                         (float(buf[2 * i + 1]) - 127.5f) / 127.5f};
    }
    // Keep the established 2048-bin squelch detector on raw input, with its
    // original scale and cadence. DC correction changes actual display/audio
    // IQ, never the detector's calibration or the number of captured samples.
    m_ddc.processSquelchSpectrum(m_iqBuffer);
    m_dcBlocker.process(std::span(m_iqBuffer.data(), m_iqBuffer.size()));
    m_ddc.processIqData(m_iqBuffer, m_pipeline->legacy(), false);
    const auto spectrum = m_ddc.takeSquelchSpectrum();
    if (!spectrum.empty()) { m_pipeline->observeSpectrum(spectrum, m_firstSample); }
    m_pipeline->process(m_firstSample, std::span(m_iqBuffer.constData(), m_iqBuffer.size()));
    m_firstSample += count;
}
} // namespace AetherSDR::rtl
