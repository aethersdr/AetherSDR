#include "QsoRecorder.h"
#include "AppSettings.h"
#include "CwRecordGate.h"
#include "AudioDeviceNegotiator.h"
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
    // recordingBlocked's argument type. Direct connections do not need this,
    // and every connection today is direct — but registering costs nothing and
    // means a future queued/cross-thread connection fails at compile time
    // rather than silently dropping the signal at runtime.
    qRegisterMetaType<RecordStartDecision>("AetherSDR::RecordStartDecision");

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
    // Finalize so an in-flight recording still gets a valid WAV header, but
    // stay SILENT: this runs during teardown, and the zero-capture diagnostic
    // below is wired to a modal dialog. Popping one while the window is being
    // destroyed is both useless and a good way to hang a quit.
    if (m_recording)
        finalizeFile(FinalizeReport::Silent);
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

// Live read of every policy input — the two settings plus the backend's own
// answer about whether it feeds us over the seam. Nothing is cached, so a
// backend swap or a settings change between two starts is picked up for free.
RecordStartDecision QsoRecorder::evaluateStart() const
{
    auto& s = AppSettings::instance();
    const bool clientSide =
        s.value("RecordingMode", "Client").toString() == "Client";
    const bool pcAudio =
        s.value("PcAudioEnabled", "True").toString() == "True";
    // No provider installed (unit tests, no radio) reads as false: the Flex
    // answer, and the one that keeps the guard active rather than silently off.
    const bool seamNative = m_backendOwnsRxAudio && m_backendOwnsRxAudio();
    return evaluateRecordStart(clientSide, pcAudio, seamNative);
}

void QsoRecorder::startRecording()
{
    beginRecording(StartTrigger::Manual);
}

void QsoRecorder::beginRecording(StartTrigger trigger)
{
    if (m_recording) return;

    // Refuse before touching the filesystem (#4629). Creating the file first
    // and discovering the silence later is precisely the failure being fixed:
    // it leaves a correctly-named, header-only WAV on disk that looks like a
    // recorder fault rather than a configuration one.
    const RecordStartDecision decision = evaluateStart();
    if (decision != RecordStartDecision::Allow) {
        // Auto-record retries on EVERY MOX rising edge, so an unchanged refusal
        // must be reported once, not once per transmission — the consumer is a
        // dialog, and one per key-down would make the radio unusable rather than
        // informative. A deliberate press always gets an answer.
        const bool alreadyReported = trigger == StartTrigger::Auto
                                     && m_lastAutoBlocked == decision;
        m_lastAutoBlocked = decision;
        if (!alreadyReported) {
            qCWarning(lcAudio) << "QsoRecorder: start refused —"
                               << (decision == RecordStartDecision::BlockedRecordingModeIsRadio
                                       ? "Radio-Side recording is selected; the radio "
                                         "records, not this client"
                                       : "client-side recording needs PC Audio "
                                         "(no RX audio stream exists)");
            emit recordingBlocked(decision);
        }
        return;
    }
    // Cleared on success so a later refusal is reported afresh rather than
    // being mistaken for a continuation of an earlier run.
    m_lastAutoBlocked.reset();

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
    // Lock-free fast path off the real-time audio thread: skip the mutex when
    // we're not recording an RX over, so a GUI thread holding m_writeMutex
    // during finalize/stop can't stall audio. The post-lock check stays
    // authoritative (m_recording/m_transmitting can flip after this read).
    if (!m_recording.load(std::memory_order_acquire)
        || m_transmitting.load(std::memory_order_acquire)
        || m_cwOverActive.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    // While transmitting the radio mutes RX (this stream would be silence), and
    // the TX monitor is recorded instead — skip RX so the two don't double-write
    // and the file stays a clean time-interleaved RX/TX stream (#3556).
    if (!m_recording || !m_file
        || m_transmitting.load(std::memory_order_acquire)
        || m_cwOverActive.load(std::memory_order_acquire)) return;
    QByteArray converted = float32ToInt16(pcm);
    m_file->write(converted);
    m_dataBytes += static_cast<quint32>(converted.size());
}

void QsoRecorder::feedTxAudio(const QByteArray& int16Stereo)
{
    // Lock-free fast path: the CW record pump calls this ~100×/s from the
    // real-time audio thread. Skip the mutex when we're not recording a TX over
    // so a GUI thread holding m_writeMutex during finalize/stop can't stall
    // audio (xrun). The post-lock check stays authoritative.
    // "Transmitting" here means the over, not the element: under break-in the
    // interlock is low in every inter-element gap, and gating on it alone
    // discarded the record pump's blocks in all of them (#4281).
    if (!m_recording.load(std::memory_order_acquire)
        || !(m_transmitting.load(std::memory_order_acquire)
             || m_cwOverActive.load(std::memory_order_acquire)))
        return;
    std::lock_guard<std::mutex> lock(m_writeMutex);
    // Only capture the TX monitor while actually transmitting (the tap can fire
    // whenever mic capture runs), so RX and TX never both write.
    if (!m_recording || !m_file
        || !(m_transmitting.load(std::memory_order_acquire)
             || m_cwOverActive.load(std::memory_order_acquire))) return;
    // The post-limiter TX monitor is already 24 kHz stereo int16 — the WAV's
    // native format — so write it directly, no float32 conversion (#3556).
    m_file->write(int16Stereo);
    m_dataBytes += static_cast<quint32>(int16Stereo.size());
}

// ── TX state tracking ───────────────────────────────────────────────────────

void QsoRecorder::onMoxChanged(bool mox)
{
    // Gate RX vs TX writes (#3556). Set before any early-return so the feed
    // slots see the correct state immediately on the TX/RX edge.
    //
    // MOX is the SOLE writer of m_transmitting. setCwOverActive deliberately
    // does not come through here: a CW over outlives the interlock by design
    // (#4281), so letting it write this flag made two writers disagree for the
    // length of the hang — long enough that a voice over started inside that
    // window had m_transmitting forced false underneath it and the rest of the
    // over was dropped, with no further MOX edge to repair it.
    m_transmitting.store(mox, std::memory_order_release);
    applyOverBookkeeping(mox);
}

// The half of onMoxChanged that is about an OVER rather than about the
// interlock: start an auto-record when one begins, run the idle timer when one
// ends. A CW over needs exactly this and must NOT touch m_transmitting (#4281).
void QsoRecorder::applyOverBookkeeping(bool overActive)
{
    // Only auto-record when in client-side recording mode
    bool clientSide = AppSettings::instance().value("RecordingMode", "Client").toString() == "Client";
    if (overActive) {
        // TX started — begin recording if auto-record is on and not already
        // recording. Auto trigger: a standing refusal is reported once, not on
        // every key-down (see beginRecording).
        if (clientSide && m_autoRecord && !m_recording)
            beginRecording(StartTrigger::Auto);

        // Reset idle timer on each over
        m_idleTimer->stop();
    } else {
        // TX ended — start the idle countdown, but only once BOTH over sources
        // are down. The CW gate-close is queued and can land inside a live
        // voice over begun during the over-hang; arming then would auto-stop
        // that recording mid-transmission (#4281). Whichever over ends last
        // arms the countdown.
        if (AetherSDR::idleCountdownShouldArm(
                m_recording,
                m_transmitting.load(std::memory_order_acquire),
                m_cwOverActive.load(std::memory_order_acquire)))
            m_idleTimer->start(m_idleTimeoutSecs * 1000);
    }
}

void QsoRecorder::setCwOverActive(bool active)
{
    // Set before delegating so both feed slots see the over immediately.
    m_cwOverActive.store(active, std::memory_order_release);
    // A CW over must still start an auto-record and run the idle timer exactly
    // as a voice over does — but it must NOT write m_transmitting, which MOX
    // owns. See onMoxChanged for what went wrong when it did.
    applyOverBookkeeping(active);
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

void QsoRecorder::finalizeFile(FinalizeReport report)
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

    const quint32 dataBytes = m_dataBytes;

    m_file->close();
    m_file->deleteLater();
    m_file = nullptr;

    qCInfo(lcAudio) << "QsoRecorder: stopped recording," << durationSecs << "seconds,"
                     << dataBytes << "bytes";
    emit recordingStopped(filePath, durationSecs);

    // A recording that captured NOTHING is the #4629 symptom, and until now it
    // was reported to the operator exactly like a good one — the file exists,
    // it is named correctly, and it holds a 44-byte header and no audio. The
    // start guard above catches the known cause (PC Audio off), so reaching
    // here means something else stranded the feed mid-session: the radio
    // dropped, the stream was torn down by another client, the backend swapped.
    // Whatever it was, say so rather than let a silent file pass for success.
    // The >= 1s floor keeps a deliberate instant start/stop from being reported
    // as a fault: under one second the recorder may legitimately not have seen a
    // single audio block yet, and an error dialog for "you stopped it
    // immediately" is noise. The tradeoff is a real blind spot — a sub-second
    // recording that captured nothing is silently accepted — but that case
    // yields no usable audio either way, whereas a false alarm on every quick
    // tap trains the operator to dismiss this dialog unread.
    if (dataBytes == 0 && durationSecs >= 1 && report == FinalizeReport::Diagnose) {
        qCWarning(lcAudio) << "QsoRecorder: recording captured no audio:" << filePath;
        emit recordingError(
            QStringLiteral("Recording captured no audio — the file contains only a "
                           "WAV header.\n\nThe RX audio stream stopped or never "
                           "started. Check that the radio is still connected and "
                           "that PC Audio is enabled.\n\n") + filePath);
    }
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

    // Prefer the device the user picked in Radio Settings > Audio
    // (m_outputDevice, seeded by MainWindow from AudioEngine).  Only
    // accept it if it is still present in the live audioOutputs() list
    // — a hotplug/unplug between selection and now would otherwise
    // strand us on a stale handle.  Mirrors ClientPuduMonitor and
    // AudioEngine::startSidetoneStream() (#3361).
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!m_outputDevice.isNull()) {
        const auto outputs = QMediaDevices::audioOutputs();
        for (const auto& d : outputs) {
            if (d.id() == m_outputDevice.id()) { dev = d; break; }
        }
    }
    if (dev.isNull()) return;

    // Negotiate the playback format via the shared factory (#3306, Phase 6b).
    // The recording is Int16, so prefer Int16 (no conversion on a normal device)
    // and fall back to Float for Float-only WASAPI mixers — the Int16->Float
    // conversion below handles that (#3231). The factory supplies the per-OS
    // preferred rate (Win/Mac 48k to dodge the WASAPI 24k resampler artifacts
    // #2120; Linux native 24k) plus the 44.1k and preferredFormat fallbacks.
    // Previously QSO playback bailed on a Float-only device; now it works.
    // Walk with isFormatSupported (trusted), mirroring ClientPuduMonitor.
    QAudioFormat fmt;
    int sinkRate = SAMPLE_RATE;
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
        c.setChannelCount(NUM_CHANNELS);
        if (dev.isFormatSupported(c)) {
            fmt = c;
            sinkRate = c.sampleRate();
            haveFormat = true;
            break;
        }
    }
    if (!haveFormat) return;

    if (!preparePlaybackPcm(sinkRate)) return;

    // Float-only WASAPI mixers reject Int16 — convert the Int16 playback PCM to
    // Float32 to match the negotiated sink format (#3231 / Phase 6b), mirroring
    // ClientPuduMonitor. Only Float32 is handled (the only non-Int16 format
    // preferredFormat() returns in practice on WASAPI/CoreAudio).
    if (fmt.sampleFormat() == QAudioFormat::Float) {
        const int samples = m_playPcm.size() / static_cast<int>(sizeof(int16_t));
        QByteArray floatPcm(samples * static_cast<int>(sizeof(float)), '\0');
        const auto* src = reinterpret_cast<const int16_t*>(m_playPcm.constData());
        auto*       dst = reinterpret_cast<float*>(floatPcm.data());
        for (int i = 0; i < samples; ++i) dst[i] = src[i] / 32768.0f;
        m_playPcm = std::move(floatPcm);
    }

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

    // Detect an immediate open failure (e.g. a WASAPI/CoreAudio device that
    // false-positives isFormatSupported() then refuses at start()) BEFORE we
    // mute live RX. If start() failed synchronously, the stateChanged handler
    // already ran stopPlayback() but it no-op'd (m_playing was still false), so
    // we must clean up here. Crucially we return *before* emitting
    // muteRxRequested(true), so a failed playback can never strand live RX in a
    // muted state (#3230 invariant) — mirrors ClientPuduMonitor::startPlayback().
    if (m_playSink->state() == QAudio::StoppedState
        && m_playSink->error() != QAudio::NoError) {
        qCWarning(lcAudio) << "QsoRecorder: playback sink failed to start (error"
                           << m_playSink->error() << ") — aborting, RX left live";
        m_playSink->disconnect(this);
        m_playSink->deleteLater();
        m_playSink = nullptr;
        if (m_playBuffer.isOpen()) m_playBuffer.close();
        return;
    }

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
