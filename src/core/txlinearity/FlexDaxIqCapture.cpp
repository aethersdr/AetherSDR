#include "FlexDaxIqCapture.h"

#include "core/LogManager.h"

#include <algorithm>

namespace AetherSDR::txlin {

FlexDaxIqCapture::FlexDaxIqCapture(int channel, QObject* parent)
    : QObject(parent), m_channel(std::clamp(channel, 1, kMaxChannels))
{
}

FlexDaxIqCapture::~FlexDaxIqCapture()
{
    // Not a courtesy: leaving the stream up would keep a DAX IQ channel
    // allocated on the radio after the analyzer is gone, and the radio has only
    // four.
    FlexDaxIqCapture::stop();
}

bool FlexDaxIqCapture::start(const CaptureConfig& config)
{
    if (m_running) {
        return true;
    }
    m_config = config;
    if (m_config.frameSamples < 64) {
        emitError("Frame size is too small to analyze");
        return false;
    }

    // Preallocate once. Nothing on the delivery path may allocate.
    m_frame.assign(std::size_t(m_config.frameSamples), std::complex<float>(0.0f, 0.0f));
    m_filled = 0;
    m_saturated = 0;
    m_gateOpen = false;
    m_actualRate = 0.0;

    const int rate = int(m_config.sampleRateHz);
    emit streamStartRequested(m_channel, rate);
    if (!m_panId.isEmpty()) {
        emit commandReady(QStringLiteral("display pan set %1 daxiq_channel=%2")
                              .arg(m_panId)
                              .arg(m_channel));
    }

    m_running = true;
    qCInfo(lcDax) << "FlexDaxIqCapture: started ch" << m_channel << "at" << rate
                  << "Hz, frame" << m_config.frameSamples << "pan" << m_panId;
    return true;
}

void FlexDaxIqCapture::stop()
{
    if (!m_running) {
        return;
    }
    // Close the gate FIRST. Any frame still in flight must not reach a consumer
    // that is tearing down.
    m_gateOpen = false;
    m_running = false;
    m_filled = 0;
    m_saturated = 0;
    emit streamStopRequested(m_channel);
    qCInfo(lcDax) << "FlexDaxIqCapture: stopped ch" << m_channel;
}

std::string FlexDaxIqCapture::describe() const
{
    QString text = m_radioDescription.isEmpty() ? QStringLiteral("FlexRadio")
                                                : m_radioDescription;
    text += QStringLiteral(" DAX IQ ch %1").arg(m_channel);
    if (m_actualRate > 0.0) {
        text += QStringLiteral(" @ %1 kHz").arg(m_actualRate / 1000.0, 0, 'f', 0);
    }
    return text.toStdString();
}

void FlexDaxIqCapture::setFrameCallback(FrameCallback callback)
{
    m_frameCallback = std::move(callback);
}

void FlexDaxIqCapture::setErrorCallback(ErrorCallback callback)
{
    m_errorCallback = std::move(callback);
}

void FlexDaxIqCapture::setGateOpen(bool open)
{
    if (m_gateOpen == open) {
        return;
    }
    m_gateOpen = open;
    // Discard whatever partial frame straddles the gate edge. Half a frame of
    // key-up transient spliced onto half a frame of steady state is not a
    // measurement of either.
    m_filled = 0;
    m_saturated = 0;
}

void FlexDaxIqCapture::noteStreamRemoved(int channel)
{
    if (channel != m_channel || !m_running) {
        return;
    }
    emitError("DAX IQ stream was removed by the radio");
}

void FlexDaxIqCapture::noteStreamRate(int channel, int sampleRateHz)
{
    if (channel != m_channel || sampleRateHz <= 0) {
        return;
    }
    m_actualRate = double(sampleRateHz);
}

void FlexDaxIqCapture::feedIqSamples(int channel, const QVector<float>& interleaved,
                                     int sampleRateHz)
{
    if (!m_running || channel != m_channel || !m_gateOpen) {
        return;
    }
    if (m_frameCallback == nullptr || sampleRateHz <= 0) {
        return;
    }

    // The radio, not the request, is authoritative about the rate. Measuring a
    // frame at the wrong assumed rate would put every frequency, and therefore
    // every product identification, in the wrong place.
    if (m_actualRate > 0.0 && double(sampleRateHz) != m_actualRate) {
        // Rate changed mid-capture: the partial frame spans two sample rates
        // and is not analyzable.
        m_filled = 0;
        m_saturated = 0;
    }
    m_actualRate = double(sampleRateHz);

    const std::size_t pairs = std::size_t(interleaved.size()) / 2;
    const float* src = interleaved.constData();
    const std::size_t frameLen = m_frame.size();

    for (std::size_t i = 0; i < pairs; ++i) {
        const float re = src[2 * i];
        const float im = src[2 * i + 1];
        if (double(re) * double(re) + double(im) * double(im) >= 0.999 * 0.999) {
            ++m_saturated;
        }
        m_frame[m_filled] = std::complex<float>(re, im);
        ++m_filled;

        if (m_filled == frameLen) {
            CaptureFrame frame;
            frame.feedback = m_frame;   // one copy, handed off to the ring
            frame.sampleRateHz = m_actualRate;
            frame.centerFreqHz = m_config.centerFreqHz;
            frame.timestampNs = 0;
            frame.clipped =
                (double(m_saturated) / double(frameLen)) > kClippedFractionThreshold;
            // reference stays disengaged — see the header for why that is
            // architectural on this backend and not a gap.
            m_frameCallback(frame);
            m_filled = 0;
            m_saturated = 0;
        }
    }
}

void FlexDaxIqCapture::emitError(const std::string& reason)
{
    qCWarning(lcDax) << "FlexDaxIqCapture:" << QString::fromStdString(reason);
    if (m_errorCallback != nullptr) {
        m_errorCallback(reason);
    }
}

}  // namespace AetherSDR::txlin
