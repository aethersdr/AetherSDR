#include "ClientPuduMonitor.h"
#include "AudioSummaryLogger.h"
#include "Resampler.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMediaDevices>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <vector>

namespace AetherSDR {

namespace {

// Standard 44-byte RIFF/WAVE header for int16 stereo 24 kHz PCM.
QByteArray makeWavHeader(quint32 payloadBytes)
{
    QByteArray h;
    h.reserve(44);
    auto u32 = [&](quint32 v) {
        h.append(char(v & 0xFF));
        h.append(char((v >> 8) & 0xFF));
        h.append(char((v >> 16) & 0xFF));
        h.append(char((v >> 24) & 0xFF));
    };
    auto u16 = [&](quint16 v) {
        h.append(char(v & 0xFF));
        h.append(char((v >> 8) & 0xFF));
    };

    h.append("RIFF", 4);
    u32(36 + payloadBytes);
    h.append("WAVE", 4);
    h.append("fmt ", 4);
    u32(16);                                                  // fmt chunk size
    u16(1);                                                    // PCM
    u16(ClientPuduMonitor::kChannels);
    u32(ClientPuduMonitor::kSampleRate);
    u32(ClientPuduMonitor::kSampleRate
        * ClientPuduMonitor::kBytesPerFrame);                  // byte rate
    u16(ClientPuduMonitor::kBytesPerFrame);                    // block align
    u16(16);                                                   // bits / sample
    h.append("data", 4);
    u32(payloadBytes);
    return h;
}

} // namespace

ClientPuduMonitor::ClientPuduMonitor(QObject* parent) : QObject(parent)
{
    // Pre-size the buffer so the audio thread never reallocates.  Qt's
    // QByteArray grows on demand; fill with zeros to force the
    // allocation now rather than on first write.
    m_buffer.fill('\0', kMaxBytes);
}

int ClientPuduMonitor::recordedMs() const noexcept
{
    return m_recordedMs;
}

void ClientPuduMonitor::startRecording()
{
    // Idempotent.  Can't record while playing — caller should stop
    // playback first; we just bail to avoid trampling playback state.
    if (m_recording.load(std::memory_order_acquire)) return;
    if (m_playing) return;

    m_writeBytes.store(0, std::memory_order_release);
    m_recordedBytes = 0;
    m_recordedMs = 0;
    m_recElapsed.restart();
    m_recording.store(true, std::memory_order_release);
    // Disconnect live RX audio for the entire record+playback cycle
    // so we don't hear radio audio mixed with our capture/playback.
    // Restored on playback end (not on record end — auto-play starts
    // immediately after, and we want the mute to persist across the
    // transition).
    emit muteRxRequested(true);
    emit recordingStarted();
}

void ClientPuduMonitor::stopRecording()
{
    // Try to flip the recording flag from true→false atomically.
    // Whoever wins (user click, auto-stop handler, or external
    // transition like mic-source change) finishes the stop work;
    // later callers no-op.
    bool wasRecording = true;
    if (!m_recording.compare_exchange_strong(wasRecording, false,
            std::memory_order_acq_rel)) {
        return;
    }

    // Audio thread sees m_recording=false before any further writes
    // via the acquire load in feedTxPostDsp().  Safe to read the
    // write count.
    m_recordedBytes = m_writeBytes.load(std::memory_order_acquire);
    m_recordedMs    = static_cast<int>(m_recElapsed.elapsed());

    writeWavFile();
    emit recordingStopped(m_recordedMs);
}

void ClientPuduMonitor::onAutoStop()
{
    // Audio thread hit the 30-s cap and flipped m_recording=false
    // itself.  Do the UI-side finalisation + emit signal.
    if (m_recordedBytes == 0) {
        m_recordedBytes = m_writeBytes.load(std::memory_order_acquire);
        m_recordedMs    = static_cast<int>(m_recElapsed.elapsed());
    }
    writeWavFile();
    emit recordingStopped(m_recordedMs);
}

bool ClientPuduMonitor::preparePlaybackPcm(int sinkRateHz)
{
    // Source payload — int16 stereo 24 kHz, exactly what we captured.
    const char* src = m_buffer.constData();
    const int srcBytes = m_recordedBytes;
    if (srcBytes <= 0) return false;

    if (sinkRateHz == kSampleRate) {
        // Passthrough — sink opens at 24 kHz so int16 bytes transfer
        // 1:1.  Most PipeWire / ALSA sinks take this on Linux.
        m_playPcm = QByteArray(src, srcBytes);
        return true;
    }

    // Sink wants a different rate — resample via r8brain.  Run L and
    // R channels independently so the stereo image is preserved
    // (Resampler::processStereoToStereo down-mixes to mono first,
    // which would collapse PUDU's Behringer-mode all-pass rotator).
    const int srcFrames = srcBytes / kBytesPerFrame;
    if (srcFrames <= 0) return false;

    std::vector<float> lIn(srcFrames);
    std::vector<float> rIn(srcFrames);
    const auto* s16 = reinterpret_cast<const int16_t*>(src);
    for (int i = 0; i < srcFrames; ++i) {
        lIn[i] = s16[i * 2]     / 32768.0f;
        rIn[i] = s16[i * 2 + 1] / 32768.0f;
    }

    // maxBlockSamples must be >= the block size we pass.  +slack so
    // r8brain's internal pipelining never truncates.
    const int blockCap = srcFrames + 16;
    Resampler lResampler(kSampleRate, sinkRateHz, blockCap);
    Resampler rResampler(kSampleRate, sinkRateHz, blockCap);
    const QByteArray lOut = lResampler.process(lIn.data(), srcFrames);
    const QByteArray rOut = rResampler.process(rIn.data(), srcFrames);

    const int lOutFrames = lOut.size() / static_cast<int>(sizeof(float));
    const int rOutFrames = rOut.size() / static_cast<int>(sizeof(float));
    const int outFrames  = std::min(lOutFrames, rOutFrames);
    if (outFrames <= 0) return false;

    m_playPcm.resize(outFrames * kBytesPerFrame);
    auto* dst = reinterpret_cast<int16_t*>(m_playPcm.data());
    const auto* lf = reinterpret_cast<const float*>(lOut.constData());
    const auto* rf = reinterpret_cast<const float*>(rOut.constData());
    for (int i = 0; i < outFrames; ++i) {
        dst[i * 2]     = static_cast<int16_t>(
            std::clamp(lf[i] * 32768.0f, -32768.0f, 32767.0f));
        dst[i * 2 + 1] = static_cast<int16_t>(
            std::clamp(rf[i] * 32768.0f, -32768.0f, 32767.0f));
    }
    return true;
}

void ClientPuduMonitor::startPlayback()
{
    if (m_playing) return;
    if (m_recordedBytes <= 0) return;

    // ── Pick an output device + format ─────────────────────────────
    // int16 stereo at the sink's native rate.  Try 24 kHz first (zero-
    // resample fast path — typical on Linux); fall back to 48 kHz
    // (typical on macOS and Windows) with a one-shot r8brain upsample
    // of the whole captured buffer up-front.
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) {
        AudioSummaryLogger::OpenFailureSummary failure;
        failure.path = QStringLiteral("Aetherial monitor playback");
        failure.backend = QStringLiteral("QAudioSink");
        failure.deviceDescription = QStringLiteral("Unavailable");
        failure.attemptedFormats = QStringLiteral("system default output");
        failure.failureReason = QStringLiteral("no audio output device");
        AudioSummaryLogger::logOpenFailure(failure);
        return;
    }

    QAudioFormat fmt;
    fmt.setChannelCount(kChannels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    fmt.setSampleRate(kSampleRate);
    int sinkRate = kSampleRate;
    bool fallbackOccurred = false;
    QStringList fallbackReasons;
    QStringList attemptedFormats;
    attemptedFormats << QStringLiteral("24000Hz 2ch Int16");

    if (!dev.isFormatSupported(fmt)) {
        fmt.setSampleRate(48000);
        sinkRate = 48000;
        fallbackOccurred = true;
        fallbackReasons << QStringLiteral("24000Hz Int16 stereo unsupported -> 48000Hz");
        attemptedFormats << QStringLiteral("48000Hz 2ch Int16");
        if (!dev.isFormatSupported(fmt)) {
            // Neither 24 kHz nor 48 kHz int16 — extremely rare.  Give up
            // quietly; playback was best-effort anyway.
            AudioSummaryLogger::OpenFailureSummary failure;
            failure.path = QStringLiteral("Aetherial monitor playback");
            failure.backend = QStringLiteral("QAudioSink");
            failure.deviceDescription = dev.description();
            failure.attemptedFormats = attemptedFormats.join(QStringLiteral("; "));
            failure.failureReason = QStringLiteral("output device supports neither 24000Hz nor 48000Hz Int16 stereo");
            failure.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
            AudioSummaryLogger::logOpenFailure(failure);
            return;
        }
    }

    if (!preparePlaybackPcm(sinkRate)) return;

    // ── QBuffer → QAudioSink (pull mode) ───────────────────────────
    // Sink pulls from QBuffer at its own cadence.  No timer, no
    // feedDecodedSpeech, no RX-buffer routing — the sink's internal
    // ring buffer absorbs scheduler jitter so the audio comes out
    // cleanly on every platform.  When QBuffer hits end-of-data the
    // sink transitions to IdleState and we stop cleanly.
    m_playBuffer.close();
    m_playBuffer.setBuffer(&m_playPcm);
    if (!m_playBuffer.open(QIODevice::ReadOnly)) return;

    m_playSink = new QAudioSink(dev, fmt, this);
    // Ask for a generous 300 ms internal ring buffer before start().
    // Qt's defaults are ~40-80 ms which is fine on Linux/macOS but
    // chops on Windows when the main event loop hiccups (painting,
    // UI events, GC) — WASAPI shared-mode pulls on a tight 10 ms
    // schedule and if we miss a refill the device inserts silence.
    // 300 ms gives ~30 pulls of margin.  Backend may clamp to the
    // device's period granularity; not an error if the effective
    // size is slightly smaller.
    m_playSink->setBufferSize(fmt.bytesForDuration(300'000));
    connect(m_playSink, &QAudioSink::stateChanged,
            this, &ClientPuduMonitor::onPlaybackSinkState);
    m_playSink->start(&m_playBuffer);
    if (m_playSink->state() == QAudio::StoppedState
        && m_playSink->error() != QAudio::NoError) {
        AudioSummaryLogger::OpenFailureSummary failure;
        failure.path = QStringLiteral("Aetherial monitor playback");
        failure.backend = QStringLiteral("QAudioSink");
        failure.deviceDescription = dev.description();
        failure.attemptedFormats = attemptedFormats.join(QStringLiteral("; "));
        failure.failureReason = QStringLiteral("QAudioSink stopped immediately after start (error %1)")
            .arg(static_cast<int>(m_playSink->error()));
        failure.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
        AudioSummaryLogger::logOpenFailure(failure);
        delete m_playSink;
        m_playSink = nullptr;
        return;
    }
    AudioSummaryLogger::AuxiliarySinkSummary summary;
    summary.sinkName = QStringLiteral("Aetherial monitor playback");
    summary.deviceDescription = dev.description();
    summary.sampleRate = fmt.sampleRate();
    summary.channelCount = fmt.channelCount();
    summary.sampleFormat = fmt.sampleFormat();
    summary.resamplingActive = sinkRate != kSampleRate;
    summary.fallbackOccurred = fallbackOccurred;
    summary.fallbackReason = fallbackReasons.join(QStringLiteral("; "));
    AudioSummaryLogger::logAuxiliarySink(summary);

    m_playing = true;
    // When playback is triggered as part of auto-play after a
    // record, muteRxRequested was already fired at record start and
    // the mute is still held, so this is a no-op.  When the user
    // triggers playback manually from idle, this is the one that
    // installs the mute.  Either way it's safe — MainWindow's
    // handler is idempotent.
    emit muteRxRequested(true);
    emit playbackStarted();
}

void ClientPuduMonitor::stopPlayback()
{
    if (!m_playing) return;
    m_playing = false;

    if (m_playSink) {
        m_playSink->stop();
        m_playSink->disconnect(this);
        m_playSink->deleteLater();
        m_playSink = nullptr;
    }
    if (m_playBuffer.isOpen()) m_playBuffer.close();

    // End of the record+playback cycle — restore live RX audio.
    emit muteRxRequested(false);
    emit playbackStopped();
}

void ClientPuduMonitor::onPlaybackSinkState(QAudio::State state)
{
    // QAudioSink transitions to IdleState when its source (QBuffer)
    // stops returning data — i.e. the full capture has been drained.
    // StoppedState also arrives on explicit stop() or device error.
    if (state == QAudio::IdleState || state == QAudio::StoppedState) {
        stopPlayback();
    }
}

void ClientPuduMonitor::feedTxPostDsp(const QByteArray& pcm) noexcept
{
    // Fast-path skip when not recording — acquire load so writes that
    // happened before we started recording are visible too.
    if (!m_recording.load(std::memory_order_acquire)) return;
    if (pcm.isEmpty()) return;

    const int writeAt = m_writeBytes.load(std::memory_order_relaxed);
    const int avail   = kMaxBytes - writeAt;
    if (avail <= 0) return;
    const int take = std::min(static_cast<int>(pcm.size()), avail);
    std::memcpy(m_buffer.data() + writeAt, pcm.constData(), take);
    const int newTotal = writeAt + take;
    m_writeBytes.store(newTotal, std::memory_order_release);

    if (newTotal >= kMaxBytes) {
        // Cap hit — stop accepting further feeds and tell the UI
        // thread to finalise + auto-start playback.
        m_recording.store(false, std::memory_order_release);
        QMetaObject::invokeMethod(this, "onAutoStop", Qt::QueuedConnection);
    }
}

void ClientPuduMonitor::writeWavFile()
{
    // /tmp/pudu_monitor.wav — overwritten on each recording.  Purely
    // for offline inspection; playback uses the in-memory buffer.
    const QString path = QDir::temp().filePath("pudu_monitor.wav");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    const QByteArray header = makeWavHeader(
        static_cast<quint32>(m_recordedBytes));
    f.write(header);
    f.write(m_buffer.constData(), m_recordedBytes);
    f.close();
}

} // namespace AetherSDR
