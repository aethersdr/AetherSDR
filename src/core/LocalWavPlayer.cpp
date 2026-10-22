#include "core/LocalWavPlayer.h"

#include "core/AudioDeviceNegotiator.h"
#include "core/LogManager.h"
#include "core/Resampler.h"
#include "core/VoiceKeyerWavDecoder.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

#include <algorithm>
#include <array>
#include <cmath>

namespace AetherSDR {

namespace {

// Mono float32 at srcRate -> mono float32 at dstRate, flushing r8brain's
// latency with silence until the expected length has come out.
QByteArray resampleMono(const QByteArray& mono, int srcRate, int dstRate)
{
    if (srcRate == dstRate)
        return mono;
    constexpr int kBlock = 4096;
    const auto* in = reinterpret_cast<const float*>(mono.constData());
    const qsizetype samples = mono.size() / qsizetype(sizeof(float));
    const qsizetype expectedBytes =
        qsizetype(double(samples) * dstRate / srcRate) * qsizetype(sizeof(float));
    Resampler resampler(srcRate, dstRate, kBlock);
    QByteArray out;
    out.reserve(expectedBytes);
    static const std::array<float, kBlock> kSilence{};
    qsizetype fed = 0;
    int flushGuard = 64;
    while (out.size() < expectedBytes && flushGuard > 0) {
        if (fed < samples) {
            const int n = int(std::min<qsizetype>(kBlock, samples - fed));
            out.append(resampler.process(in + fed, n));
            fed += n;
        } else {
            out.append(resampler.process(kSilence.data(), kBlock));
            --flushGuard;
        }
    }
    out.truncate(std::min(out.size(), expectedBytes));
    return out;
}

} // namespace

LocalWavPlayer::LocalWavPlayer(QObject* parent)
    : QObject(parent)
{
}

LocalWavPlayer::~LocalWavPlayer()
{
    stop();
}

bool LocalWavPlayer::play(const QString& path, QString& error)
{
    stop();

    QByteArray mono;
    int fileRate = 0;
    if (!VoiceKeyerWavDecoder::decodeToMonoFloat(path, mono, fileRate, error))
        return false;

    // The device the operator picked, if it is still present; else the default.
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!m_outputDevice.isNull()) {
        for (const auto& d : QMediaDevices::audioOutputs()) {
            if (d.id() == m_outputDevice.id()) { dev = d; break; }
        }
    }
    if (dev.isNull()) {
        error = QStringLiteral("No audio output device is available.");
        return false;
    }

    // Same negotiation as QsoRecorder::startPlayback (#3306, Phase 6b).
    QAudioFormat fmt;
    bool haveFormat = false;
    const QList<QAudioFormat> ladder = AudioDeviceNegotiator::formatLadder(
        dev, AudioFormatNegotiator::Direction::Output,
        AudioFormatNegotiator::ResamplerPolicy::PreservePan,
        AudioFormatNegotiator::hostTargetOs(),
        AudioFormatNegotiator::kInternalRate,
        /*bluetoothHfp=*/false, /*preferredRateOverride=*/0,
        AudioFormatNegotiator::FormatPreference::Int16First);
    for (const QAudioFormat& cand : ladder) {
        QAudioFormat c = cand;
        c.setChannelCount(2);
        if (dev.isFormatSupported(c)) { fmt = c; haveFormat = true; break; }
    }
    if (!haveFormat) {
        error = QStringLiteral("The audio output device refuses every supported format.");
        return false;
    }

    // Mono at the device rate, duplicated to both channels in the sink's format.
    const QByteArray atRate = resampleMono(mono, fileRate, fmt.sampleRate());
    const auto* src = reinterpret_cast<const float*>(atRate.constData());
    const qsizetype frames = atRate.size() / qsizetype(sizeof(float));
    if (fmt.sampleFormat() == QAudioFormat::Float) {
        m_pcm.resize(frames * 2 * qsizetype(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(m_pcm.data());
        for (qsizetype i = 0; i < frames; ++i)
            dst[2 * i] = dst[2 * i + 1] = src[i];
    } else {
        m_pcm.resize(frames * 2 * qsizetype(sizeof(qint16)));
        auto* dst = reinterpret_cast<qint16*>(m_pcm.data());
        for (qsizetype i = 0; i < frames; ++i) {
            const auto v = static_cast<qint16>(std::lround(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f));
            dst[2 * i] = dst[2 * i + 1] = v;
        }
    }

    m_buffer.close();
    m_buffer.setBuffer(&m_pcm);
    if (!m_buffer.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot prepare the audio for playback.");
        return false;
    }

    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(fmt.bytesForDuration(300'000));  // WASAPI jitter headroom, as QsoRecorder
    connect(m_sink, &QAudioSink::stateChanged, this, &LocalWavPlayer::onSinkState);
    m_sink->start(&m_buffer);

    // An open that fails synchronously must not mute RX (#3230 invariant).
    if (m_sink->state() == QAudio::StoppedState && m_sink->error() != QAudio::NoError) {
        qCWarning(lcAudio) << "LocalWavPlayer: output failed to start (error"
                           << m_sink->error() << ") — RX left live";
        m_sink->disconnect(this);
        m_sink->deleteLater();
        m_sink = nullptr;
        m_buffer.close();
        error = QStringLiteral("The audio output device failed to start.");
        return false;
    }

    m_playing = true;
    emit muteRxRequested(true);
    return true;
}

void LocalWavPlayer::stop()
{
    if (!m_playing)
        return;
    m_playing = false;
    if (m_sink) {
        m_sink->stop();
        m_sink->disconnect(this);
        m_sink->deleteLater();
        m_sink = nullptr;
    }
    m_buffer.close();
    emit muteRxRequested(false);
    emit finished();
}

void LocalWavPlayer::onSinkState(QAudio::State state)
{
    if (state == QAudio::IdleState || state == QAudio::StoppedState)
        stop();
}

} // namespace AetherSDR
