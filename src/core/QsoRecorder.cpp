#include "QsoRecorder.h"
#include "AppSettings.h"
#include "LogManager.h"
#include "Resampler.h"
#include "../models/SliceModel.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDir>
#include <QMediaDevices>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <vector>

namespace AetherSDR {

QsoRecorder::QsoRecorder(QObject* parent)
    : QObject(parent)
{
    // Default recording directory: ~/Documents/AetherSDR/Recordings
    m_recordingDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                     + "/AetherSDR/Recordings";

    // Restore settings
    auto& s = AppSettings::instance();
    m_recordingDir = s.value("QsoRecordingDir", m_recordingDir).toString();
    m_idleTimeoutSecs = s.value("QsoRecordingIdleTimeout", 120).toInt();
    m_autoRecord = s.value("QsoRecordingAutoRecord", false).toBool();
    m_includeDate = s.value("QsoRecordingIncludeDate", true).toBool();
    m_includeTime = s.value("QsoRecordingIncludeTime", true).toBool();
    m_includeFreq = s.value("QsoRecordingIncludeFreq", true).toBool();
    m_includeMode = s.value("QsoRecordingIncludeMode", true).toBool();

    // Idle timer — fires when no TX activity for m_idleTimeoutSecs
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    connect(m_idleTimer, &QTimer::timeout, this, [this]() {
        if (m_recording) {
            qCInfo(lcAudio) << "QsoRecorder: idle timeout, stopping recording";
            stopRecording();
        }
    });
}

QsoRecorder::~QsoRecorder()
{
    if (m_recording)
        finalizeFile();
}

void QsoRecorder::setRecordingDir(const QString& path)
{
    m_recordingDir = path;
    AppSettings::instance().setValue("QsoRecordingDir", path);
}

void QsoRecorder::setIdleTimeoutSecs(int secs)
{
    m_idleTimeoutSecs = qBound(10, secs, 3600);
    AppSettings::instance().setValue("QsoRecordingIdleTimeout", m_idleTimeoutSecs);
}

void QsoRecorder::setAutoRecordEnabled(bool on)
{
    m_autoRecord = on;
    AppSettings::instance().setValue("QsoRecordingAutoRecord", on);
}

void QsoRecorder::setCallsign(const QString& call)
{
    m_callsign = call.trimmed().toUpper();
}

void QsoRecorder::setSlice(SliceModel* slice)
{
    m_slice = slice;
}

int QsoRecorder::recordingDurationSecs() const
{
    if (!m_recording) return 0;
    return static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()));
}

// ── Manual control ──────────────────────────────────────────────────────────

void QsoRecorder::startRecording()
{
    if (m_recording) return;
    // Re-read settings in case they changed via Radio Setup dialog
    auto& s = AppSettings::instance();
    m_recordingDir = s.value("QsoRecordingDir", m_recordingDir).toString();
    m_idleTimeoutSecs = s.value("QsoRecordingIdleTimeout", "120").toInt();
    m_autoRecord = s.value("QsoRecordingAutoRecord", "False").toString() == "True";
    startFile();
}

void QsoRecorder::stopRecording()
{
    if (!m_recording) return;
    m_idleTimer->stop();
    finalizeFile();
}

// ── Audio feeds ─────────────────────────────────────────────────────────────

// Convert float32 stereo PCM to int16 stereo PCM for WAV output.
static QByteArray float32ToInt16(const QByteArray& pcm)
{
    const int numFloats = pcm.size() / static_cast<int>(sizeof(float));
    QByteArray out(numFloats * static_cast<int>(sizeof(qint16)), Qt::Uninitialized);
    const float* src = reinterpret_cast<const float*>(pcm.constData());
    qint16* dst = reinterpret_cast<qint16*>(out.data());
    for (int i = 0; i < numFloats; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<qint16>(clamped * 32767.0f);
    }
    return out;
}

void QsoRecorder::feedRxAudio(const QByteArray& pcm)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_recording || !m_file) return;
    QByteArray converted = float32ToInt16(pcm);
    m_file->write(converted);
    m_dataBytes += static_cast<quint32>(converted.size());
}

void QsoRecorder::feedTxAudio(const QByteArray& pcm)
{
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_recording || !m_file) return;
    QByteArray converted = float32ToInt16(pcm);
    m_file->write(converted);
    m_dataBytes += static_cast<quint32>(converted.size());
}

// ── TX state tracking ───────────────────────────────────────────────────────

void QsoRecorder::onMoxChanged(bool mox)
{
    // Only auto-record when in client-side recording mode
    bool clientSide = AppSettings::instance().value("RecordingMode", "Radio").toString() == "Client";
    if (mox) {
        // TX started — begin recording if auto-record is on and not already recording
        if (clientSide && m_autoRecord && !m_recording)
            startRecording();

        // Reset idle timer on each TX
        m_idleTimer->stop();
    } else {
        // TX ended — start idle countdown
        if (m_recording)
            m_idleTimer->start(m_idleTimeoutSecs * 1000);
    }
}

// ── File management ─────────────────────────────────────────────────────────

void QsoRecorder::startFile()
{
    // Capture metadata from active slice at recording start
    if (m_slice) {
        m_freqMhz = m_slice->frequency();
        m_mode = m_slice->mode();
    } else {
        m_freqMhz = 0.0;
        m_mode.clear();
    }

    m_startTime = QDateTime::currentDateTimeUtc();
    m_dataBytes = 0;

    // Ensure directory exists
    QDir dir(m_recordingDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit recordingError("Cannot create recording directory: " + m_recordingDir);
            return;
        }
    }

    QString filePath = m_recordingDir + "/" + buildFilename();

    m_file = new QFile(filePath, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        emit recordingError("Cannot create recording file: " + m_file->errorString());
        delete m_file;
        m_file = nullptr;
        return;
    }

    writeWavHeader();
    m_recording = true;

    qCInfo(lcAudio) << "QsoRecorder: started recording to" << filePath;
    emit recordingStarted(filePath);
}

void QsoRecorder::finalizeFile()
{
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_recording = false;
    }

    if (!m_file) return;

    patchWavHeader();
    QString filePath = m_file->fileName();
    m_lastRecordingPath = filePath;
    int durationSecs = static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()));

    m_file->close();
    m_file->deleteLater();
    m_file = nullptr;

    qCInfo(lcAudio) << "QsoRecorder: stopped recording," << durationSecs << "seconds,"
                     << m_dataBytes << "bytes";
    emit recordingStopped(filePath, durationSecs);
}

QString QsoRecorder::buildFilename() const
{
    QStringList parts;

    if (m_includeDate)
        parts << m_startTime.toString("yyyy-MM-dd");

    if (m_includeTime)
        parts << m_startTime.toString("HHmmss") + "Z";

    if (m_includeFreq && m_freqMhz > 0.0) {
        // Format frequency: e.g. 14.200 MHz → "14.200"
        parts << QString::number(m_freqMhz, 'f', 3) + "MHz";
    }

    if (m_includeMode && !m_mode.isEmpty())
        parts << sanitizeForPath(m_mode);

    if (!m_callsign.isEmpty())
        parts << sanitizeForPath(m_callsign);

    if (parts.isEmpty())
        parts << "QSO";

    return parts.join("_") + ".wav";
}

QString QsoRecorder::sanitizeForPath(const QString& s)
{
    // Replace any character that isn't A-Z or 0-9 with '_'. m_callsign is
    // already uppercased + trimmed by setCallsign(), so this class is
    // sufficient. Keeps '/' in portable-suffix callsigns (KK7GWY/P) from
    // punching into a subdirectory or failing QFile::open() on Windows.
    // Also applied to m_mode so a future digital sub-mode label with '/'
    // can't re-introduce the same bug class.
    static const QRegularExpression re(QStringLiteral("[^A-Z0-9]"));
    QString out = s;
    out.replace(re, QStringLiteral("_"));
    return out;
}

void QsoRecorder::writeWavHeader()
{
    // Write a placeholder WAV header (44 bytes). The data size fields
    // will be patched in finalizeFile() once we know the total size.
    char header[WAV_HEADER_SIZE] = {};

    const int byteRate = SAMPLE_RATE * NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    const int blockAlign = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);

    // RIFF header
    memcpy(header + 0, "RIFF", 4);
    // header[4..7] = file size - 8 (patched later)
    memcpy(header + 8, "WAVE", 4);

    // fmt sub-chunk
    memcpy(header + 12, "fmt ", 4);
    qToLittleEndian<quint32>(16, header + 16);               // sub-chunk size
    qToLittleEndian<quint16>(1, header + 20);                 // audio format (1 = PCM)
    qToLittleEndian<quint16>(NUM_CHANNELS, header + 22);      // channels
    qToLittleEndian<quint32>(SAMPLE_RATE, header + 24);       // sample rate
    qToLittleEndian<quint32>(byteRate, header + 28);          // byte rate
    qToLittleEndian<quint16>(blockAlign, header + 32);        // block align
    qToLittleEndian<quint16>(BITS_PER_SAMPLE, header + 34);   // bits per sample

    // data sub-chunk
    memcpy(header + 36, "data", 4);
    // header[40..43] = data size (patched later)

    m_file->write(header, WAV_HEADER_SIZE);
}

void QsoRecorder::patchWavHeader()
{
    if (!m_file || !m_file->isOpen()) return;

    // Seek back and patch the two size fields in the WAV header
    m_file->seek(4);
    quint32 riffSize = m_dataBytes + WAV_HEADER_SIZE - 8;
    char buf[4];
    qToLittleEndian<quint32>(riffSize, buf);
    m_file->write(buf, 4);

    m_file->seek(40);
    qToLittleEndian<quint32>(m_dataBytes, buf);
    m_file->write(buf, 4);
}

// ── Playback ───────────────────────────────────────────────────────────────

bool QsoRecorder::preparePlaybackPcm(int sinkRateHz)
{
    QFile f(m_lastRecordingPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    f.seek(WAV_HEADER_SIZE);
    QByteArray raw = f.readAll();
    f.close();
    if (raw.isEmpty()) return false;

    if (sinkRateHz == SAMPLE_RATE) {
        m_playPcm = std::move(raw);
        return true;
    }

    // Resample L and R independently to preserve stereo image.
    static constexpr int kBytesPerFrame = NUM_CHANNELS * (BITS_PER_SAMPLE / 8);
    const int srcFrames = raw.size() / kBytesPerFrame;
    if (srcFrames <= 0) return false;

    std::vector<float> lIn(srcFrames), rIn(srcFrames);
    const auto* s16 = reinterpret_cast<const qint16*>(raw.constData());
    for (int i = 0; i < srcFrames; ++i) {
        lIn[i] = s16[i * 2]     / 32768.0f;
        rIn[i] = s16[i * 2 + 1] / 32768.0f;
    }

    Resampler lResampler(SAMPLE_RATE, sinkRateHz, srcFrames + 16);
    Resampler rResampler(SAMPLE_RATE, sinkRateHz, srcFrames + 16);
    const QByteArray lOut = lResampler.process(lIn.data(), srcFrames);
    const QByteArray rOut = rResampler.process(rIn.data(), srcFrames);

    const int outFrames = std::min(lOut.size() / static_cast<int>(sizeof(float)),
                                   rOut.size() / static_cast<int>(sizeof(float)));
    if (outFrames <= 0) return false;

    m_playPcm.resize(outFrames * kBytesPerFrame);
    auto* dst = reinterpret_cast<qint16*>(m_playPcm.data());
    const auto* lf = reinterpret_cast<const float*>(lOut.constData());
    const auto* rf = reinterpret_cast<const float*>(rOut.constData());
    for (int i = 0; i < outFrames; ++i) {
        dst[i * 2]     = static_cast<qint16>(std::clamp(lf[i] * 32768.0f, -32768.0f, 32767.0f));
        dst[i * 2 + 1] = static_cast<qint16>(std::clamp(rf[i] * 32768.0f, -32768.0f, 32767.0f));
    }
    return true;
}

void QsoRecorder::startPlayback()
{
    if (m_playing || m_lastRecordingPath.isEmpty()) return;

    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;

    QAudioFormat fmt;
    fmt.setChannelCount(NUM_CHANNELS);
    fmt.setSampleFormat(QAudioFormat::Int16);
    fmt.setSampleRate(SAMPLE_RATE);
    int sinkRate = SAMPLE_RATE;

    if (!dev.isFormatSupported(fmt)) {
        fmt.setSampleRate(48000);
        sinkRate = 48000;
        if (!dev.isFormatSupported(fmt)) return;
    }

    if (!preparePlaybackPcm(sinkRate)) return;

    m_playBuffer.close();
    m_playBuffer.setBuffer(&m_playPcm);
    if (!m_playBuffer.open(QIODevice::ReadOnly)) return;

    m_playSink = new QAudioSink(dev, fmt, this);
    // 300 ms ring buffer: absorbs Windows WASAPI jitter (default ~40 ms starves
    // on event-loop hiccups and inserts silence).  Backend may clamp to its
    // period granularity; not an error if the effective size differs.
    m_playSink->setBufferSize(fmt.bytesForDuration(300'000));
    connect(m_playSink, &QAudioSink::stateChanged,
            this, &QsoRecorder::onPlaybackSinkState);
    m_playSink->start(&m_playBuffer);

    m_playing = true;
    emit muteRxRequested(true);
    emit playbackStarted();
}

void QsoRecorder::stopPlayback()
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

    emit muteRxRequested(false);
    emit playbackStopped();
}

void QsoRecorder::onPlaybackSinkState(QAudio::State state)
{
    if (state == QAudio::IdleState || state == QAudio::StoppedState) {
        stopPlayback();
    }
}

} // namespace AetherSDR
