#include "QuindarLocalSink.h"
#include "AudioDeviceNegotiator.h"
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

    // Negotiate the output format via the shared factory (#3306). The Quindar
    // tone is generated in Float, so walk only the Float rungs of the ladder —
    // which gives this sink, in one place, the per-OS preferred rate plus the
    // 44.1 kHz and device-preferredFormat fallbacks it previously lacked (it
    // failed outright on a 44.1k-only output, with only 48k + preferred tried).
    //
    // Each rung is tried with a real QAudioSink::start(), not
    // isFormatSupported() (#4641): on Windows/WASAPI that query answers
    // against the shared-mode mix format and false-negatives on class-
    // compliant multichannel USB interfaces (Akai EIE and similar) that
    // accept the format fine once actually opened — matches AudioEngine's
    // RX sink, which never trusted the query to begin with.
    QStringList attemptedFormats;
    QAudioFormat fmt;
    QString lastOpenError;
    const QList<QAudioFormat> ladder = AudioDeviceNegotiator::formatLadder(
        dev, AudioFormatNegotiator::Direction::Output,
        AudioFormatNegotiator::ResamplerPolicy::RegenerateAtRate);
    for (const QAudioFormat& cand : ladder) {
        if (cand.sampleFormat() != QAudioFormat::Float)
            continue;   // the tone is generated in Float

        // onTimerTick() assumes a stereo frame layout, so pin the channel
        // count here (matches CwSidetoneQAudioSink) rather than trusting
        // the ladder to keep returning 2ch for every rung.
        QAudioFormat c = cand;
        c.setChannelCount(2);
        attemptedFormats << QStringLiteral("%1Hz %2ch Float")
            .arg(c.sampleRate()).arg(c.channelCount());

        auto* sink = new QAudioSink(dev, c, this);
        // Match CwSidetoneQAudioSink's 50 ms buffer — required to keep
        // Pulse/PipeWire push-mode happy.  Net latency ~25 ms; fine for
        // 250 ms Quindar tones.
        sink->setBufferSize(c.bytesForDuration(50'000));
        QIODevice* io = sink->start();
        if (!io) {
            lastOpenError = QString::number(static_cast<int>(sink->error()));
            delete sink;
            continue;
        }

        fmt = c;
        m_sink = sink;
        m_device = io;
        if (c.sampleRate() != 48000) {
            fallbackOccurred = true;
            fallbackReasons << QStringLiteral("negotiated %1Hz Float").arg(c.sampleRate());
        }
        break;
    }
    if (!m_sink) {
        qCWarning(lcAudio) << "QuindarLocalSink: device supports no Float stereo rate"
                           << dev.description();
        AudioSummaryLogger::OpenFailureSummary failure;
        failure.path = QStringLiteral("Quindar local sink");
        failure.backend = QStringLiteral("QAudioSink");
        failure.deviceDescription = dev.description();
        failure.attemptedFormats = attemptedFormats.join(QStringLiteral("; "));
        failure.failureReason = lastOpenError.isEmpty()
            ? QStringLiteral("no Float stereo rung supported in the negotiation ladder")
            : QStringLiteral("QAudioSink::start returned null (error %1)").arg(lastOpenError);
        failure.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
        AudioSummaryLogger::logOpenFailure(failure);
        return false;
    }
    m_actualRate = fmt.sampleRate();
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
