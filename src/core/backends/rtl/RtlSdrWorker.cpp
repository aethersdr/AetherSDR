#include "core/backends/rtl/RtlSdrWorker.h"
#include "core/backends/rtl/RtlSdrBackend.h"

#include <QDebug>
#include <rtl-sdr.h>
#include <climits>

namespace AetherSDR::rtl {

static constexpr uint32_t kRtlBufLength = 16384;   // 16KB per USB transfer
static constexpr uint32_t kRtlBufNum    = 15;      // 15 buffers

RtlSdrWorker::RtlSdrWorker(struct rtlsdr_dev* dev, QObject* parent)
    : QThread(parent)
    , m_dev(dev)
{
    // Relay DDC signals across thread boundary to main thread
    connect(&m_ddc, &RtlSdrDdc::spectrumFrameReady,
            this, &RtlSdrWorker::spectrumFrameReady);
    connect(&m_ddc, &RtlSdrDdc::waterfallRowReady,
            this, &RtlSdrWorker::waterfallRowReady);
    connect(&m_ddc, &RtlSdrDdc::audioFrameReady,
            this, &RtlSdrWorker::audioFrameReady);
}

RtlSdrWorker::~RtlSdrWorker()
{
    if (!stopReading()) {
        qFatal("RtlSdrWorker destroyed while its USB reader is still running");
    }
    if (m_dev) {
        rtlsdr_close(static_cast<rtlsdr_dev_t*>(m_dev));
        m_dev = nullptr;
    }
}

void RtlSdrWorker::startReading()
{
    if (isRunning()) {
        return;
    }

    m_stopRequested = false;
    m_readerRunning = false;
    start();
}

bool RtlSdrWorker::stopReading()
{
    m_stopRequested = true;

    // Cancellation issued just before librtlsdr enters read_async can be lost.
    // Repeat it while waiting so that narrow startup race cannot leave a
    // QThread alive at destruction or tempt the backend to close a live handle.
    for (int attempt = 0; isRunning() && attempt < 50; ++attempt) {
        if (m_dev) {
            rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
        }
        if (wait(100)) {
            break;
        }
    }

    m_readerRunning = false;
    if (isRunning()) {
        qWarning() << "RtlSdrWorker: USB reader did not stop after repeated cancellation";
        return false;
    }
    return true;
}

// ── Async USB control transfer dispatch ─────────────────────────────────────
// These methods are called from the main thread. They store the requested
// value in an atomic, then signal the callback to cancel read_async so the
// run() loop can apply the USB control transfer outside the callback context.
//
// Why: rtlsdr_set_center_freq / rtlsdr_set_direct_sampling / rtlsdr_set_tuner_gain
// are USB control transfers that silently fail or deadlock when called from
// within the rtlsdr_read_async callback (the USB handle is busy with bulk IQ
// transfers). The standard librtlsdr pattern is: cancel → retune → restart.

void RtlSdrWorker::setCenterFrequency(uint32_t hz)
{
    m_pendingCenterHz.store(static_cast<int64_t>(hz), std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::setTunerGain(int gainTenths)
{
    m_pendingGainTenths.store(gainTenths, std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::setDirectSampling(int mode)
{
    m_pendingDirectSampling.store(mode, std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::setPpmCorrection(int ppm)
{
    m_pendingPpm.store(ppm, std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::setSampleRate(uint32_t rate)
{
    const uint32_t clampedRate = RtlSdrBackend::clampSampleRate(rate);
    m_pendingSampleRate.store(static_cast<int64_t>(clampedRate), std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::setOffsetTuning(int enable)
{
    m_pendingOffsetTuning.store(enable, std::memory_order_relaxed);
    m_retuneRequested.store(true, std::memory_order_release);
    if (m_readerRunning.load() && m_dev) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(m_dev));
    }
}

void RtlSdrWorker::run()
{
    if (!m_dev) {
        emit readError(QStringLiteral("Device handle is null"));
        return;
    }

    rtlsdr_dev_t* dev = static_cast<rtlsdr_dev_t*>(m_dev);

    // ── Cancel / retune / restart loop ──────────────────────────────────────
    // rtlsdr_read_async blocks until rtlsdr_cancel_async() is called.
    // USB control transfers (frequency, gain, direct sampling) CANNOT be
    // issued from within the read_async callback — the USB handle is busy.
    // Instead, the setters call cancel_async to break out, we apply the
    // control transfers here (outside the callback), then restart read_async.
    while (!m_stopRequested.load()) {
        const int resetRc = rtlsdr_reset_buffer(dev);
        if (resetRc < 0) {
            emit readError(QStringLiteral("rtlsdr_reset_buffer failed with error %1")
                               .arg(resetRc));
            break;
        }

        // Publish this before entering read_async, not from its first callback:
        // an empty or stalled device may never deliver a callback, but stop and
        // retune still have to cancel the blocking call.
        m_readerRunning.store(true, std::memory_order_release);
        if (m_stopRequested.load(std::memory_order_acquire)) {
            m_readerRunning.store(false, std::memory_order_release);
            break;
        }

        int rc = rtlsdr_read_async(dev,
                                   &RtlSdrWorker::rtlsdrCallback,
                                   this,
                                   kRtlBufNum,
                                   kRtlBufLength);

        m_readerRunning = false;

        if (m_stopRequested.load()) {
            break;
        }

        // read_async returned because of a retune request — apply controls
        if (m_retuneRequested.exchange(false, std::memory_order_acq_rel)) {
            applyPendingControls();
            continue;  // restart read_async with new settings
        }

        // Unexpected return — real error
        if (rc < 0) {
            emit readError(QStringLiteral("rtlsdr_read_async failed with error %1").arg(rc));
            break;
        }
    }
}

void RtlSdrWorker::rtlsdrCallback(unsigned char* buf, uint32_t len, void* ctx)
{
    auto* worker = static_cast<RtlSdrWorker*>(ctx);
    if (!worker) {
        return;
    }

    if (worker->m_stopRequested.load()) {
        if (worker->m_dev) {
            rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(worker->m_dev));
        }
        return;
    }

    // If a retune was requested, cancel read_async so the run() loop can
    // apply USB control transfers outside this callback context.
    if (worker->m_retuneRequested.load(std::memory_order_acquire)) {
        if (worker->m_dev) {
            rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(worker->m_dev));
        }
        return;  // Don't process this buffer — stale frequency data
    }

    worker->handleCallback(buf, len);
}

void RtlSdrWorker::handleCallback(unsigned char* buf, uint32_t len)
{
    if (!buf || len == 0) {
        return;
    }

    // ── No USB control transfers here ───────────────────────────────────────
    // USB control transfers (rtlsdr_set_center_freq, etc.) MUST NOT be called
    // from within the rtlsdr_read_async callback — they silently fail because
    // the USB handle is busy with bulk IQ transfers. All control transfers are
    // applied in applyPendingControls(), called from run() after read_async
    // returns.

    // ── Convert IQ and process ──────────────────────────────────────────────
    const uint32_t numSamples = len / 2;
    if (static_cast<uint32_t>(m_iqBuffer.size()) != numSamples) {
        m_iqBuffer.resize(numSamples);
    }

    // Convert unsigned 8-bit [0..255] interleaved [I, Q] to float complex [-1.0..+1.0]
    for (uint32_t i = 0; i < numSamples; ++i) {
        const float real = (static_cast<float>(buf[2 * i])     - 127.5f) / 127.5f;
        const float imag = (static_cast<float>(buf[2 * i + 1]) - 127.5f) / 127.5f;
        m_iqBuffer[i] = std::complex<float>(real, imag);
    }

    // Process DDC (FFT spectrum & demodulation) on this worker thread
    m_ddc.processIqData(m_iqBuffer);
}

// ── USB control transfer application ────────────────────────────────────────
// Called from run() OUTSIDE the rtlsdr_read_async callback context, after
// read_async has returned. This is the only safe place to issue USB control
// transfers on the RTL-SDR device handle.

void RtlSdrWorker::applyPendingControls()
{
    rtlsdr_dev_t* dev = static_cast<rtlsdr_dev_t*>(m_dev);
    if (!dev) {
        return;
    }

    // Direct sampling MUST be applied BEFORE center frequency, because tuning
    // to HF (< 24 MHz) requires direct sampling mode 2 before setting frequency,
    // and tuning to VHF/UHF (>= 24 MHz) requires direct sampling mode 0 before
    // the tuner PLL can be programmed.
    int pendingDs = m_pendingDirectSampling.exchange(-1, std::memory_order_relaxed);
    if (pendingDs >= 0) {
        int rc = rtlsdr_set_direct_sampling(dev, pendingDs);
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_direct_sampling failed, rc =" << rc;
            emit controlFailed(QStringLiteral("direct_sampling"),
                               QStringLiteral("rtlsdr_set_direct_sampling failed: %1").arg(rc));
        } else {
            qDebug() << "RtlSdrWorker: direct sampling set to" << pendingDs;
            emit controlApplied(QStringLiteral("direct_sampling"),
                                rtlsdr_get_direct_sampling(dev));
        }
    }

    int64_t pendingHz = m_pendingCenterHz.exchange(-1, std::memory_order_relaxed);
    if (pendingHz >= 0) {
        int rc = rtlsdr_set_center_freq(dev, static_cast<uint32_t>(pendingHz));
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_center_freq failed, rc =" << rc
                       << "requested" << pendingHz << "Hz";
            emit controlFailed(QStringLiteral("center_frequency"),
                               QStringLiteral("rtlsdr_set_center_freq failed: %1").arg(rc));
        } else {
            qDebug() << "RtlSdrWorker: center freq set to" << pendingHz << "Hz";
            emit controlApplied(QStringLiteral("center_frequency"),
                                rtlsdr_get_center_freq(dev));
        }
    }

    int pendingGain = m_pendingGainTenths.exchange(INT_MIN, std::memory_order_relaxed);
    if (pendingGain != INT_MIN) {
        rtlsdr_set_tuner_gain_mode(dev, 1);
        int rc = rtlsdr_set_tuner_gain(dev, pendingGain);
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_tuner_gain failed, rc =" << rc;
            emit controlFailed(QStringLiteral("gain"),
                               QStringLiteral("rtlsdr_set_tuner_gain failed: %1").arg(rc));
        } else {
            qDebug() << "RtlSdrWorker: tuner gain set to" << pendingGain << "tenths dB";
            emit controlApplied(QStringLiteral("gain"), rtlsdr_get_tuner_gain(dev));
        }
    }

    int pendingPpm = m_pendingPpm.exchange(INT_MIN, std::memory_order_relaxed);
    if (pendingPpm != INT_MIN) {
        int rc = rtlsdr_set_freq_correction(dev, pendingPpm);
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_freq_correction failed, rc =" << rc;
            emit controlFailed(QStringLiteral("ppm"),
                               QStringLiteral("rtlsdr_set_freq_correction failed: %1").arg(rc));
        } else {
            qDebug() << "RtlSdrWorker: freq correction set to" << pendingPpm << "ppm";
            emit controlApplied(QStringLiteral("ppm"), rtlsdr_get_freq_correction(dev));
        }
    }

    int64_t pendingRate = m_pendingSampleRate.exchange(-1, std::memory_order_relaxed);
    if (pendingRate > 0) {
        int rc = rtlsdr_set_sample_rate(dev, static_cast<uint32_t>(pendingRate));
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_sample_rate failed, rc =" << rc;
            emit controlFailed(QStringLiteral("sample_rate"),
                               QStringLiteral("rtlsdr_set_sample_rate failed: %1").arg(rc));
        } else {
            const uint32_t actualRate = rtlsdr_get_sample_rate(dev);
            qDebug() << "RtlSdrWorker: sample rate set to" << actualRate << "Hz";
            m_ddc.setSampleRate(static_cast<double>(actualRate));
            emit controlApplied(QStringLiteral("sample_rate"), actualRate);
        }
    }

    int pendingOffset = m_pendingOffsetTuning.exchange(-1, std::memory_order_relaxed);
    if (pendingOffset >= 0) {
        int rc = rtlsdr_set_offset_tuning(dev, pendingOffset);
        if (rc < 0) {
            qWarning() << "RtlSdrWorker: rtlsdr_set_offset_tuning failed, rc =" << rc;
            emit controlFailed(QStringLiteral("offset_tuning"),
                               QStringLiteral("rtlsdr_set_offset_tuning failed: %1").arg(rc));
        } else {
            qDebug() << "RtlSdrWorker: offset tuning set to" << pendingOffset;
            emit controlApplied(QStringLiteral("offset_tuning"),
                                rtlsdr_get_offset_tuning(dev));
        }
    }
}

}  // namespace AetherSDR::rtl
