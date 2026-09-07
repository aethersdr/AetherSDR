#include "CwSidetoneQAudioSink.h"
#include "AudioDeviceNegotiator.h"
#include "CwSidetoneGenerator.h"
#include "LogManager.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>

namespace AetherSDR {

CwSidetoneQAudioSink::CwSidetoneQAudioSink(QObject* parent)
    : QObject(parent)
{}

CwSidetoneQAudioSink::~CwSidetoneQAudioSink()
{
    stop();
}

bool CwSidetoneQAudioSink::start(const QAudioDevice& device,
                                 int desiredRateHz,
                                 CwSidetoneGenerator* generator)
{
    if (m_sink) return true;
    if (!generator) return false;

    // Fall back if device doesn't support the desired rate.  Some devices
    // (DAX, HFP, USB cards) only support a subset.
    QAudioDevice dev = device;
    m_fallbackOccurred = false;
    m_fallbackReason.clear();
    if (dev.isNull()) {
        dev = QMediaDevices::defaultAudioOutput();
        m_fallbackOccurred = true;
        m_fallbackReason = QStringLiteral("requested output unavailable -> system default");
    }
    m_deviceDescription = dev.description();

    // Negotiate the output format via the shared factory (#3306, Phase 6c). The
    // sidetone generator retunes to the negotiated rate (RegenerateAtRate) and
    // the tick handles both Float and Int16, so we walk the default Float-first
    // ladder — Int16 is the VB-Audio / Int16-only-WASAPI fallback (#2629). The
    // factory supplies the per-OS preferred rate plus the 44.1k and
    // preferredFormat fallbacks in one place.
    //
    // Each rung is tried with a real QAudioSink::start(), not
    // isFormatSupported() (#4641): on Windows/WASAPI that query answers
    // against the shared-mode mix format and false-negatives on class-
    // compliant multichannel USB interfaces (Akai EIE and similar) that
    // accept the format fine once actually opened — matches AudioEngine's
    // RX sink, which never trusted the query to begin with.
    int chosenRate = 0;
    QAudioFormat::SampleFormat chosenFmt = QAudioFormat::Unknown;
    const QList<QAudioFormat> ladder = AudioDeviceNegotiator::formatLadder(
        dev, AudioFormatNegotiator::Direction::Output,
        AudioFormatNegotiator::ResamplerPolicy::RegenerateAtRate);
    for (const QAudioFormat& cand : ladder) {
        QAudioFormat c = cand;
        c.setChannelCount(2);
        if (c.sampleFormat() != QAudioFormat::Float && c.sampleFormat() != QAudioFormat::Int16)
            continue;   // the sidetone tick only knows Float / Int16

        auto* sink = new QAudioSink(dev, c, this);
        // 50 ms buffer — Pulse/PipeWire happily honour ≥40 ms; <30 ms causes
        // pull-mode Idle/Active flapping and audible chop.  Real perceived
        // latency stays low (~25 ms typical) because we keep the buffer about
        // half-full via the 2 ms timer, not because the buffer itself is small.
        sink->setBufferSize(c.bytesForDuration(50'000));
        QIODevice* io = sink->start();
        if (!io) {
            delete sink;
            continue;
        }

        chosenRate = c.sampleRate();
        chosenFmt = c.sampleFormat();
        m_sink = sink;
        m_device = io;
        break;
    }
    if (chosenRate == 0) {
        qCWarning(lcAudio) << "CwSidetoneQAudioSink: no supported float/int16-stereo rate on device"
                           << dev.description();
        return false;
    }
    m_actualRate   = chosenRate;
    m_sampleFormat = chosenFmt;
    if (chosenRate != (desiredRateHz > 0 ? desiredRateHz : 48000)
        || chosenFmt != QAudioFormat::Float) {
        m_fallbackOccurred = true;
        const QString detail = QStringLiteral("negotiated %1Hz %2")
            .arg(chosenRate)
            .arg(chosenFmt == QAudioFormat::Float ? QStringLiteral("Float")
                                                   : QStringLiteral("Int16"));
        m_fallbackReason = m_fallbackReason.isEmpty()
            ? detail
            : m_fallbackReason + QStringLiteral("; ") + detail;
    }

    m_generator = generator;
    m_generator->setSampleRateHz(chosenRate);

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(2);
        connect(m_timer, &QTimer::timeout,
                this, &CwSidetoneQAudioSink::onTimerTick);
    }
    m_timer->start();

    qCInfo(lcAudio) << "CwSidetoneQAudioSink: started"
                    << "rate=" << chosenRate << "Hz"
                    << "format=" << (chosenFmt == QAudioFormat::Float ? "Float" : "Int16")
                    << "buffer=" << m_sink->bufferSize() << "bytes (push, 2ms timer)";
    return true;
}

void CwSidetoneQAudioSink::onTimerTick()
{
    if (!m_sink || !m_device || !m_generator) return;
    const qsizetype freeBytes = m_sink->bytesFree();
    if (freeBytes <= 0) return;
    const qsizetype frameBytes = (m_sampleFormat == QAudioFormat::Float)
                                     ? qsizetype(2 * sizeof(float))
                                     : qsizetype(2 * sizeof(int16_t));
    const qsizetype byteCount = (freeBytes / frameBytes) * frameBytes;
    if (byteCount == 0) return;
    const int frames = static_cast<int>(byteCount / frameBytes);

    if (m_sampleFormat == QAudioFormat::Float) {
        QByteArray chunk(byteCount, '\0');
        m_generator->process(reinterpret_cast<float*>(chunk.data()), frames);
        m_edgeProbe.scan(reinterpret_cast<const float*>(chunk.constData()), frames);
        m_device->write(chunk);
    } else {
        const qsizetype scratchBytes = qsizetype(frames) * 2 * qsizetype(sizeof(float));
        if (m_scratch.size() < scratchBytes) m_scratch.resize(scratchBytes);
        auto* fbuf = reinterpret_cast<float*>(m_scratch.data());
        m_generator->process(fbuf, frames);
        m_edgeProbe.scan(fbuf, frames);

        QByteArray chunk(byteCount, Qt::Uninitialized);
        auto* ibuf = reinterpret_cast<int16_t*>(chunk.data());
        const int samples = frames * 2;
        for (int i = 0; i < samples; ++i) {
            float s = fbuf[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            ibuf[i] = static_cast<int16_t>(s * 32767.0f);
        }
        m_device->write(chunk);
    }
}

void CwSidetoneQAudioSink::stop()
{
    if (m_timer && m_timer->isActive()) m_timer->stop();
    m_edgeProbe.dump("QAudioSink", m_actualRate);
    if (m_sink) {
        auto* sink = m_sink;
        m_sink = nullptr;
        m_device = nullptr;
        if (sink->state() != QAudio::StoppedState) sink->stop();
        sink->deleteLater();
    }
    m_generator = nullptr;
    m_actualRate = 0;
    m_deviceDescription.clear();
    m_fallbackOccurred = false;
    m_fallbackReason.clear();
    m_sampleFormat = QAudioFormat::Float;
    m_scratch.clear();
}

} // namespace AetherSDR
