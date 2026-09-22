#include "core/backends/rtl/RtlSdrWorker.h"

#include <QDebug>
#include <rtl-sdr.h>

namespace AetherSDR::rtl {
namespace {
constexpr std::uint32_t kRtlBufLength = 16384;
constexpr std::uint32_t kRtlBufNum = 15;
using T = RtlCaptureTransaction;

class UsbDevice final : public RtlSdrWorker::Device {
public:
    explicit UsbDevice(rtlsdr_dev_t* device) : m_device(device) {}
    ~UsbDevice() override { if (m_device) { rtlsdr_close(m_device); } }
    bool set(T::Control control, std::int64_t value) override
    {
        if (!m_device) { return false; }
        // Several librtlsdr versions return -2 for unchanged PPM or unsupported
        // offset tuning, even when disabled. Skip only a matching readable
        // value; the transaction still verifies the complete final readback.
        switch (control) {
        case T::Control::DirectSampling:
            return rtlsdr_get_direct_sampling(m_device) == value
                || rtlsdr_set_direct_sampling(m_device, static_cast<int>(value)) == 0;
        case T::Control::SampleRate:
            return rtlsdr_set_sample_rate(m_device, static_cast<std::uint32_t>(value)) == 0;
        case T::Control::Ppm:
            return rtlsdr_get_freq_correction(m_device) == value
                || rtlsdr_set_freq_correction(m_device, static_cast<int>(value)) == 0;
        case T::Control::OffsetTuning:
            return rtlsdr_get_offset_tuning(m_device) == value
                || rtlsdr_set_offset_tuning(m_device, static_cast<int>(value)) == 0;
        case T::Control::Center:
            return rtlsdr_set_center_freq(m_device, static_cast<std::uint32_t>(value)) == 0;
        case T::Control::Gain:
            return rtlsdr_set_tuner_gain_mode(m_device, 1) == 0
                && rtlsdr_set_tuner_gain(m_device, static_cast<int>(value)) == 0
                && rtlsdr_set_agc_mode(m_device, 0) == 0;
        }
        return false;
    }
    std::optional<T::Hardware> read() override
    {
        if (!m_device) { return {}; }
        return T::Hardware{rtlsdr_get_center_freq(m_device), rtlsdr_get_sample_rate(m_device),
            rtlsdr_get_direct_sampling(m_device), rtlsdr_get_offset_tuning(m_device),
            rtlsdr_get_freq_correction(m_device), rtlsdr_get_tuner_gain(m_device)};
    }
    bool resetBuffer() override { return m_device && rtlsdr_reset_buffer(m_device) == 0; }
    int readAsync(Callback callback, void* context) override
    {
        return rtlsdr_read_async(m_device, callback, context, kRtlBufNum, kRtlBufLength);
    }
    void cancelAsync() override { if (m_device) { rtlsdr_cancel_async(m_device); } }
private:
    rtlsdr_dev_t* m_device;
};
} // namespace

RtlSdrWorker::RtlSdrWorker(struct rtlsdr_dev* dev, QObject* parent)
    : RtlSdrWorker(std::make_unique<UsbDevice>(dev), parent)
{
}

RtlSdrWorker::RtlSdrWorker(std::unique_ptr<Device> device, QObject* parent)
    : QThread(parent), m_device(std::move(device))
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
        [this](const QByteArray& pcm) {
            emit audioFrameReady(m_applied.session, m_applied.revision, pcm);
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
    return true;
}

bool RtlSdrWorker::submit(const Transaction::Work& work)
{
    // M1a deliberately keeps the legacy single DDC. The complete-set owner
    // already supports admission for future banks, but this adapter does not.
    if (work.target.receivers.size() != 1
        || m_command.load(std::memory_order_acquire) != Command::Idle) { return false; }
    m_work = work;
    // Receiver-only result storage is prepared off the callback. At the block
    // boundary only the numeric DDC configuration and release flag change.
    m_result = Transaction::Result{work.token, Transaction::ResultCode::Applied, work.target, work.operation};
    m_command.store(work.hardwareChanged ? Command::Hardware : Command::Receiver,
                    std::memory_order_release);
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
    // Also called by the backend timer: retry across the narrow readAsync entry
    // race, including a device that never delivers its first callback.
    if (m_command.load(std::memory_order_acquire) == Command::Hardware
        && m_readerRunning.load(std::memory_order_acquire) && m_device) {
        m_device->cancelAsync();
    }
}

void RtlSdrWorker::applyDdc(const Transaction::State& state)
{
    const Transaction::Receiver& receiver = state.receivers.front();
    m_ddc.applyCapture(state.hardware.sampleRateHz, state.hardware.centerHz,
                      receiver.passband.carrierHz, receiver.mode,
                      static_cast<int>(receiver.passband.filterLowHz),
                      static_cast<int>(receiver.passband.filterHighHz));
    m_applied = state.token;
}

void RtlSdrWorker::applyHardware()
{
    m_command.store(Command::Applying, std::memory_order_release);
    m_result = Transaction::execute(*m_work, *m_device);
    if (m_result->actual) { applyDdc(*m_result->actual); }
    else { m_applied = {}; }
    m_command.store(Command::Complete, std::memory_order_release);
}

void RtlSdrWorker::run()
{
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
    if (command == Command::Receiver) {
        worker->applyDdc(worker->m_work->target);
        worker->m_command.store(Command::Complete, std::memory_order_release);
    }
    worker->handleCallback(buf, len);
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
    m_ddc.processIqData(m_iqBuffer);
}
} // namespace AetherSDR::rtl
