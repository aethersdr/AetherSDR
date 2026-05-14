#include "CwSidetoneQAudioSink.h"
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
    if (dev.isNull()) dev = QMediaDevices::defaultAudioOutput();

    const int kCandidateRates[] = { desiredRateHz > 0 ? desiredRateHz : 48000,
                                    48000, 44100, 24000 };
    QAudioFormat fmt;
    fmt.setChannelCount(2);

    // Probe Float first (the historical path), then Int16. VB-Audio Virtual
    // Cable and other Int16-only WASAPI endpoints fail the Float probe but
    // succeed at Int16; without the fallback the sidetone sink silently
    // refuses to open against SmartSDR-parity output devices (issue #2629).
    int chosenRate = 0;
    QAudioFormat::SampleFormat chosenFmt = QAudioFormat::Unknown;
    for (auto sf : { QAudioFormat::Float, QAudioFormat::Int16 }) {
        fmt.setSampleFormat(sf);
        for (int rate : kCandidateRates) {
            fmt.setSampleRate(rate);
            if (dev.isFormatSupported(fmt)) { chosenRate = rate; chosenFmt = sf; break; }
        }
        if (chosenRate) break;
    }
    if (chosenRate == 0) {
        qCWarning(lcAudio) << "CwSidetoneQAudioSink: no supported float/int16-stereo rate on device"
                           << dev.description();
        return false;
    }
    fmt.setSampleFormat(chosenFmt);
    fmt.setSampleRate(chosenRate);
    m_actualRate   = chosenRate;
    m_sampleFormat = chosenFmt;

    m_sink = new QAudioSink(dev, fmt, this);
    // 50 ms buffer — Pulse/PipeWire happily honour ≥40 ms; <30 ms causes
    // pull-mode Idle/Active flapping and audible chop.  Real perceived
    // latency stays low (~25 ms typical) because we keep the buffer about
    // half-full via the 2 ms timer, not because the buffer itself is small.
    constexpr int kSidetoneBufferMs = 50;
    const int sampleBytes = (chosenFmt == QAudioFormat::Float)
                                ? static_cast<int>(sizeof(float))
                                : static_cast<int>(sizeof(int16_t));
    const int sidetoneBufBytes =
        chosenRate * 2 * sampleBytes * kSidetoneBufferMs / 1000;
    m_sink->setBufferSize(sidetoneBufBytes);

    m_generator = generator;
    m_generator->setSampleRateHz(chosenRate);

    m_device = m_sink->start();
    if (!m_device) {
        qCWarning(lcAudio) << "CwSidetoneQAudioSink: sink failed to start at" << chosenRate;
        delete m_sink;
        m_sink = nullptr;
        m_generator = nullptr;
        return false;
    }

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
        m_device->write(chunk);
    } else {
        const qsizetype scratchBytes = qsizetype(frames) * 2 * qsizetype(sizeof(float));
        if (m_scratch.size() < scratchBytes) m_scratch.resize(scratchBytes);
        auto* fbuf = reinterpret_cast<float*>(m_scratch.data());
        m_generator->process(fbuf, frames);

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
    if (m_sink) {
        auto* sink = m_sink;
        m_sink = nullptr;
        m_device = nullptr;
        if (sink->state() != QAudio::StoppedState) sink->stop();
        sink->deleteLater();
    }
    m_generator = nullptr;
    m_actualRate = 0;
    m_sampleFormat = QAudioFormat::Float;
    m_scratch.clear();
}

} // namespace AetherSDR
