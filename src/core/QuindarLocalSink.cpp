#include "QuindarLocalSink.h"
#include "AudioSummaryLogger.h"
#include "ClientQuindarTone.h"
#include "LogManager.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMediaDevices>
#include <QStringList>
#include <QTimer>

namespace AetherSDR {

QuindarLocalSink::QuindarLocalSink(QObject* parent)
    : QObject(parent)
{}

QuindarLocalSink::~QuindarLocalSink()
{
    stop();
}

bool QuindarLocalSink::start(const QAudioDevice& device,
                              ClientQuindarTone* tone)
{
    if (m_sink) return true;
    if (!tone) return false;

    bool fallbackOccurred = false;
    QStringList fallbackReasons;
    QAudioDevice dev = device;
    if (dev.isNull()) {
        dev = QMediaDevices::defaultAudioOutput();
        fallbackOccurred = true;
        fallbackReasons << QStringLiteral("requested output unavailable -> system default");
    }
    if (dev.isNull()) {
        qCWarning(lcAudio) << "QuindarLocalSink: no audio output device";
        AudioSummaryLogger::OpenFailureSummary failure;
        failure.path = QStringLiteral("Quindar local sink");
        failure.backend = QStringLiteral("QAudioSink");
        failure.deviceDescription = QStringLiteral("Unavailable");
        failure.attemptedFormats = QStringLiteral("system default output");
        failure.failureReason = QStringLiteral("no audio output device");
        failure.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
        AudioSummaryLogger::logOpenFailure(failure);
        return false;
    }

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    QStringList attemptedFormats;
    attemptedFormats << QStringLiteral("48000Hz 2ch Float");
    if (!dev.isFormatSupported(fmt)) {
        fallbackOccurred = true;
        fallbackReasons << QStringLiteral("48000Hz Float stereo unsupported -> preferred output format");
        fmt = dev.preferredFormat();
        if (fmt.sampleFormat() != QAudioFormat::Float) {
            fmt.setSampleFormat(QAudioFormat::Float);
        }
        fmt.setChannelCount(2);
        attemptedFormats << QStringLiteral("%1Hz %2ch %3")
            .arg(fmt.sampleRate())
            .arg(fmt.channelCount())
            .arg(AudioSummaryLogger::sampleFormatName(fmt.sampleFormat()));
    }
    m_actualRate = fmt.sampleRate();

    m_sink = new QAudioSink(dev, fmt, this);
    // Match CwSidetoneQAudioSink's 50 ms buffer — required to keep
    // Pulse/PipeWire push-mode happy.  Net latency ~25 ms; fine for
    // 250 ms Quindar tones.
    const int bytesPerFrame = fmt.channelCount() * sizeof(float);
    m_sink->setBufferSize(50 * fmt.sampleRate() / 1000 * bytesPerFrame);
    m_device = m_sink->start();
    if (!m_device) {
        qCWarning(lcAudio) << "QuindarLocalSink: failed to start sink";
        AudioSummaryLogger::OpenFailureSummary failure;
        failure.path = QStringLiteral("Quindar local sink");
        failure.backend = QStringLiteral("QAudioSink");
        failure.deviceDescription = dev.description();
        failure.attemptedFormats = attemptedFormats.join(QStringLiteral("; "));
        failure.failureReason = QStringLiteral("QAudioSink::start returned null (error %1)")
            .arg(static_cast<int>(m_sink->error()));
        failure.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
        AudioSummaryLogger::logOpenFailure(failure);
        delete m_sink;
        m_sink = nullptr;
        return false;
    }
    m_tone = tone;

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setInterval(10);
        connect(m_timer, &QTimer::timeout, this,
                &QuindarLocalSink::onTimerTick);
    }
    m_timer->start();

    qCInfo(lcAudio) << "QuindarLocalSink: started"
                    << "rate=" << m_actualRate
                    << "buffer=" << m_sink->bufferSize() << "bytes";
    AudioSummaryLogger::AuxiliarySinkSummary summary;
    summary.sinkName = QStringLiteral("Quindar local sink");
    summary.deviceDescription = dev.description();
    summary.sampleRate = fmt.sampleRate();
    summary.channelCount = fmt.channelCount();
    summary.sampleFormat = fmt.sampleFormat();
    summary.resamplingActive = fmt.sampleRate() != 48000;
    summary.fallbackOccurred = fallbackOccurred;
    summary.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
    AudioSummaryLogger::logAuxiliarySink(summary);
    return true;
}

void QuindarLocalSink::onTimerTick()
{
    if (!m_sink || !m_device || !m_tone) return;
    const qsizetype freeBytes = m_sink->bytesFree();
    if (freeBytes <= 0) return;
    constexpr qsizetype frameBytes = 2 * sizeof(float);
    const qsizetype byteCount = (freeBytes / frameBytes) * frameBytes;
    if (byteCount == 0) return;

    QByteArray chunk(byteCount, '\0');
    const int frames = static_cast<int>(byteCount / frameBytes);
    auto* buf = reinterpret_cast<float*>(chunk.data());

    // ClientQuindarTone::processSidetone fills the buffer with the
    // generated Quindar audio when the atomic phase is Engaging or
    // Disengaging, leaves it as zeros otherwise.  Identical waveform
    // to what the TX-stream insertion produces, so the operator hears
    // exactly what the radio is transmitting.
    m_tone->processSidetone(buf, frames, static_cast<double>(m_actualRate));

    m_device->write(chunk);
}

void QuindarLocalSink::stop()
{
    if (m_timer && m_timer->isActive()) m_timer->stop();
    if (m_sink) {
        auto* sink = m_sink;
        m_sink = nullptr;
        m_device = nullptr;
        if (sink->state() != QAudio::StoppedState)
            sink->stop();
        delete sink;
    }
    m_tone = nullptr;
}

} // namespace AetherSDR
