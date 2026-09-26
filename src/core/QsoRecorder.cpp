#include "QsoRecorder.h"
#include "AppSettings.h"
#include "CwRecordGate.h"
#include "AudioDeviceNegotiator.h"
#include "LogManager.h"
#include "QsoWavPlayback.h"
#include "../models/SliceModel.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDir>
#include <QFileInfo>
#include <QMediaDevices>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

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
    stopPlayback();
    // Finalize so an in-flight recording still gets a valid WAV header, but
    // stay SILENT: this runs during teardown, and the zero-capture diagnostic
    // below is wired to a modal dialog. Popping one while the window is being
    // destroyed is both useless and a good way to hang a quit.
    if (m_recording || m_writeFailurePending)
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
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!m_recording || !m_fileFormat) {
        return 0;
    }
    return static_cast<int>(m_dataBytes / m_fileFormat->byteRate());
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
    // A feed-side failure stops accepting audio immediately, but its owner-
    // thread finalization is deliberately queued so the audio thread never
    // seeks, flushes, closes, or emits. Do not let a new run reuse m_file
    // before that finalization has retired it.
    if (m_recording || m_writeFailurePending) {
        return;
    }

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

int QsoRecorder::stopRecording()
{
    if (!m_recording && !m_writeFailurePending) {
        return 0;
    }
    m_idleTimer->stop();
    return finalizeFile();
}

// ── Audio feeds ─────────────────────────────────────────────────────────────

namespace {
bool validFixedPcm(const QByteArray& pcm, bool floating)
{
    const qsizetype sampleBytes = floating ? sizeof(float) : sizeof(qint16);
    const qsizetype frameBytes = sampleBytes * 2;
    if (pcm.isEmpty() || pcm.size() % frameBytes != 0
        || pcm.size() / frameBytes > QsoPcmConverter::kMaxInputFrames) {
        // #5648: a recording must not lose audio quietly. No producer wired
        // today can emit either shape, so this is the "impossible" case
        // reporting itself rather than a block disappearing without a trace.
        qCWarning(lcAudio) << "QsoRecorder: dropped a malformed fixed-rate block —"
                           << pcm.size() << "bytes, frame size" << frameBytes;
        return false;
    }
    if (floating) {
        for (qsizetype offset = 0; offset < pcm.size(); offset += sizeof(float)) {
            float sample;
            std::memcpy(&sample, pcm.constData() + offset, sizeof(sample));
            if (!std::isfinite(sample)) {
                return false;
            }
        }
    }
    return true;
}
} // namespace

void QsoRecorder::feedRxFrame(const PcmFrame& frame)
{
    if (!frame.current() || frame.stream().purpose != PcmPurpose::Speaker) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_writeMutex);
    // Select one normalized speaker source before touching its replay cursor.
    // A different current producer is not an implicit source-switch request.
    if (!frame.current()) {
        return;
    }
    if (m_rxObservation.current() && m_rxObservation.stream() != frame.stream()) {
        // One file, one source, deliberately -- a different current producer is
        // not an implicit switch request. Say it once, though: if the selected
        // epoch is ever live but no longer producing, this silently locks out
        // the replacement and RX recording just stops with nothing logged.
        if (!m_foreignSourceWarned) {
            m_foreignSourceWarned = true;
            qCWarning(lcAudio)
                << "QsoRecorder: ignoring a second speaker producer; recording "
                   "stays on the first while its epoch is live";
        }
        return;
    }
    if (!m_rxGate.accept(frame)) {
        return;
    }
    const bool sourceChanged = m_rxObservation.stream() != frame.stream();
    m_rxObservation = frame;
    if (sourceChanged) {
        m_foreignSourceWarned = false;
    }
    if (!m_recording || !m_file || m_transmitting || m_cwOverActive) {
        return;
    }
    if (!selectPcmSegment(PcmSource::TypedRx, frame.stream().format, frame)) {
        return;
    }
    const QByteArrayView input(reinterpret_cast<const char*>(frame.samples().constData()),
                              frame.samples().size() * sizeof(float));
    QByteArray converted;
    if (!m_pcmConverter->process(input, converted)) {
        queueWriteFailure(QStringLiteral("RX PCM conversion failed"));
        return;
    }
    m_nextRxSample = frame.firstSample() + frame.frameCount();
    writeConvertedPcm(converted);
}

void QsoRecorder::feedRxAudio(const QByteArray& pcm)
{
    feedFixedPcm(pcm, PcmSource::LegacyRx);
}

void QsoRecorder::feedTxAudio(const QByteArray& pcm)
{
    feedFixedPcm(pcm, PcmSource::Voice);
}

void QsoRecorder::feedCwAudio(const QByteArray& pcm)
{
    feedFixedPcm(pcm, PcmSource::Cw);
}

void QsoRecorder::feedFixedPcm(const QByteArray& pcm, PcmSource source)
{
    const bool rx = source == PcmSource::LegacyRx;
    const auto admitted = [this, rx]() {
        const bool txOver = m_transmitting.load(std::memory_order_acquire)
            || m_cwOverActive.load(std::memory_order_acquire);
        return m_recording.load(std::memory_order_acquire) && (rx ? !txOver : txOver);
    };
    // Keep fixed-rate real-time callers off the mutex while capture is inactive.
    if (!admitted()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_writeMutex);
    if (!admitted() || !m_file || (rx && m_rxObservation.current())
        || !validFixedPcm(pcm, rx)) {
        return;
    }
    if (!selectPcmSegment(source, PcmFormat{})) {
        return;
    }
    QByteArray converted;
    if (!m_pcmConverter->process(pcm, converted)) {
        queueWriteFailure(QStringLiteral("Fixed-rate PCM conversion failed"));
        return;
    }
    writeConvertedPcm(converted);
}

bool QsoRecorder::selectPcmSegment(PcmSource source, PcmFormat format,
                                   const PcmFrame& frame)
{
    const bool change = source != m_pcmSource
        || (source == PcmSource::TypedRx
            && (m_pcmEpoch.stream() != frame.stream() || frame.discontinuity()
                || frame.firstSample() != m_nextRxSample));
    if (change || !m_pcmConverter) {
        if (!finishPcmSegment()) {
            return false;
        }
        const bool floating = source == PcmSource::TypedRx || source == PcmSource::LegacyRx;
        const QsoPcmConverter::Configuration configuration{
            format.sampleRateHz, format.channels(),
            floating ? QsoPcmConverter::InputEncoding::Float32Native
                     : QsoPcmConverter::InputEncoding::Int16Native,
            m_fileFormat->sampleRateHz(), 2,
            QsoPcmConverter::OutputEncoding::Int16LittleEndian};
        m_pcmConverter = std::make_unique<QsoPcmConverter>(configuration);
        m_pcmSource = source;
        if (!m_pcmConverter->isValid()) {
            queueWriteFailure(QStringLiteral("Unsupported recording PCM format"));
            return false;
        }
    }
    m_pcmEpoch = frame;
    return true;
}

bool QsoRecorder::finishPcmSegment()
{
    if (m_pcmConverter && !m_writeFailurePending
        && (m_pcmSource != PcmSource::TypedRx || m_pcmEpoch.current())) {
        QByteArray tail;
        if (!m_pcmConverter->finish(tail)) {
            queueWriteFailure(QStringLiteral("Recording PCM tail conversion failed"));
        } else {
            writeConvertedPcm(tail);
        }
    }
    // Revoked delayed samples are abandoned; already accepted file bytes stay.
    m_pcmConverter.reset();
    m_pcmSource = PcmSource::None;
    m_pcmEpoch = {};
    m_nextRxSample = 0;
    return !m_writeFailurePending;
}

bool QsoRecorder::writeConvertedPcm(const QByteArray& pcm)
{
    if (m_beforePcmWriteForTest) {
        m_beforePcmWriteForTest();
    }
    // Final acquire-load is the PCM write-admission point. A revocation that
    // precedes it rejects pending converted output; one after it cannot undo
    // bytes accepted by QFile. Stop and file replacement share this mutex.
    if (m_pcmSource == PcmSource::TypedRx && !m_pcmEpoch.current()) {
        m_pcmConverter->discard();
        return true;
    }
    if (pcm.isEmpty()) {
        return true;
    }
    if (static_cast<quint64>(pcm.size()) > QsoRecordingFormat::kMaxDataBytes - m_dataBytes) {
        queueWriteFailure(QStringLiteral("Recording reached the RIFF size limit"));
        return false;
    }
    const qint64 requested = pcm.size();
    const qint64 accepted = std::clamp(writeFile(pcm.constData(), requested), qint64{0}, requested);
    m_dataBytes += static_cast<quint32>(accepted);
    if (accepted != requested) {
        const QString deviceError = m_file->errorString();
        queueWriteFailure(QStringLiteral("PCM audio write failed")
            + (deviceError.isEmpty() ? QString{} : QStringLiteral(": ") + deviceError));
        return false;
    }
    return true;
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
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        const bool wasOver = m_transmitting || m_cwOverActive;
        m_transmitting.store(mox, std::memory_order_release);
        if (wasOver != (mox || m_cwOverActive)) {
            finishPcmSegment();
        }
    }
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
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        const bool wasActive = m_cwOverActive;
        const bool wasOver = m_transmitting || wasActive;
        m_cwOverActive.store(active, std::memory_order_release);
        // A delayed CW edge can arrive during a MOX-admitted voice over.
        // Keep that voice filter continuous; actual incoming CW PCM retires
        // it in selectPcmSegment. A CW segment still drains at its own edge.
        if (wasActive != active
            && (wasOver != (m_transmitting || active) || m_pcmSource == PcmSource::Cw)) {
            finishPcmSegment();
        }
    }
    // A CW over must still start an auto-record and run the idle timer exactly
    // as a voice over does — but it must NOT write m_transmitting, which MOX
    // owns. See onMoxChanged for what went wrong when it did.
    applyOverBookkeeping(active);
}

// ── File management ─────────────────────────────────────────────────────────

void QsoRecorder::startFile()
{
    std::unique_lock<std::mutex> lock(m_writeMutex);
    Q_ASSERT(!m_file);
    // QDir("") resolves to the working directory. A missing configured path
    // must fail visibly rather than silently putting recordings there.
    if (m_recordingDir.isEmpty()) {
        lock.unlock();
        emit recordingError(QStringLiteral("Cannot create recording directory: path is empty"));
        return;
    }

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
    m_fileFormat.emplace(m_rxObservation);
    m_pcmConverter.reset();
    m_pcmSource = PcmSource::None;
    m_pcmEpoch = {};

    // Ensure directory exists
    QDir dir(m_recordingDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            lock.unlock();
            emit recordingError("Cannot create recording directory: " + m_recordingDir);
            return;
        }
    }

    const QString filename = buildFilename();
    const QFileInfo filenameInfo(filename);
    const QString filenameStem = filenameInfo.completeBaseName();
    const QString filenameSuffix = filenameInfo.suffix();
    constexpr int kMaxFilenameAttempts = 1000;

    QString filePath;
    QString openError;
    bool filenameAttemptsExhausted = false;
    for (int attempt = 0; attempt < kMaxFilenameAttempts; ++attempt) {
        const QString candidateName = attempt == 0
            ? filename
            : filenameStem + QStringLiteral("_") + QString::number(attempt)
                + QStringLiteral(".") + filenameSuffix;
        filePath = dir.filePath(candidateName);

        std::unique_ptr<QFile> file = std::make_unique<QFile>(filePath);
        if (file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            file->setParent(this);
            m_file = file.release();
            break;
        }

        openError = file->errorString();
        // NewOnly makes this check a classification after the atomic create
        // attempt, never an exists-before-open TOCTOU window. QFileInfo::exists
        // is false for a dangling link, so preserve it as an occupied name too.
        const QFileInfo candidateInfo(filePath);
        if (!candidateInfo.exists() && !candidateInfo.isSymbolicLink()) {
            break;
        }
        filenameAttemptsExhausted = attempt + 1 == kMaxFilenameAttempts;
    }

    if (!m_file) {
        const QString suffix = filenameAttemptsExhausted
            ? QStringLiteral("all %1 filename candidates are occupied")
                  .arg(kMaxFilenameAttempts)
            : openError;
        lock.unlock();
        emit recordingError(QStringLiteral("Cannot create recording file: ") + suffix);
        return;
    }

    if (!writeWavHeader()) {
        const QString error = QStringLiteral("Cannot initialize recording file: ")
                              + m_file->errorString();
        m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
        m_lastRecordingPath.clear();
        lock.unlock();
        emit recordingError(error);
        return;
    }

    m_recordingGeneration.fetch_add(1, std::memory_order_acq_rel);
    m_recording = true;
    lock.unlock();

    qCInfo(lcAudio) << "QsoRecorder: started recording to" << filePath;
    emit recordingStarted(filePath);
}

int QsoRecorder::finalizeFile(FinalizeReport report)
{
    bool writeFailed = false;
    bool finalized = false;
    QString writeFailure;
    QString finalizeError;
    QString filePath;
    int durationSecs = 0;
    qint64 elapsedSecs = 0;
    quint32 dataBytes = 0;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        m_recording = false;
        finishPcmSegment();
        writeFailed = m_writeFailurePending.exchange(false, std::memory_order_acq_rel);
        writeFailure = m_pendingWriteError;
        m_pendingWriteError.clear();
        if (!m_file) {
            return 0;
        }

        finalized = patchWavHeader();
        finalizeError = m_file->errorString();
        filePath = m_file->fileName();
        durationSecs = static_cast<int>(m_dataBytes / m_fileFormat->byteRate());
        elapsedSecs = m_startTime.secsTo(QDateTime::currentDateTimeUtc());
        dataBytes = m_dataBytes;

        m_file->close();
        // QFile can report a native close error after a successful flush.
        // Preserve an earlier header failure, otherwise sample the final
        // device result before releasing the handle and advertising playback.
        if (finalized && m_file->error() != QFileDevice::NoError) {
            finalized = false;
            finalizeError = m_file->errorString();
        }
        m_file->deleteLater();
        m_file = nullptr;

        if (finalized && !writeFailed) {
            m_lastRecordingPath = filePath;
        } else {
            // Do not leave a prior successful path advertised after a failed
            // run. Filename reuse can otherwise make it name this very file.
            m_lastRecordingPath.clear();
        }
    }

    qCInfo(lcAudio) << "QsoRecorder: stopped recording," << durationSecs << "seconds,"
                     << dataBytes << "bytes" << (finalized && !writeFailed ? "" : "(write failed)");
    const QPointer<QsoRecorder> guard(this);
    emit recordingStopped(filePath, durationSecs);
    if (!guard) {
        return durationSecs;
    }

    if (!finalized || writeFailed) {
        if (report == FinalizeReport::Diagnose) {
            const QString detail = writeFailed ? writeFailure
                : QStringLiteral("Could not finalize WAV recording")
                      + (finalizeError.isEmpty() ? QString{} : QStringLiteral(": ") + finalizeError);
            qCWarning(lcAudio) << "QsoRecorder:" << detail << filePath;
            emit recordingError(QStringLiteral("Recording write failed: %1\n\n%2")
                                    .arg(detail, filePath));
        }
        return durationSecs;
    }

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
    if (dataBytes == 0 && elapsedSecs >= 1 && report == FinalizeReport::Diagnose) {
        qCWarning(lcAudio) << "QsoRecorder: recording captured no audio:" << filePath;
        emit recordingError(
            QStringLiteral("Recording captured no audio — the file contains only a "
                           "WAV header.\n\nThe RX audio stream stopped or never "
                           "started. Check that the radio is still connected and "
                           "that PC Audio is enabled.\n\n") + filePath);
    }
    return durationSecs;
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

void QsoRecorder::finalizeWriteFailure(quint64 generation)
{
    // Explicit stop/destruction may have already finalized the failed file.
    // A stale queued callback must never touch a newer recording.
    if (generation != m_recordingGeneration.load(std::memory_order_acquire)
        || !m_writeFailurePending.load(std::memory_order_acquire)) {
        return;
    }
    m_idleTimer->stop();
    finalizeFile();
}

qint64 QsoRecorder::writeFile(const char* data, qint64 size)
{
    if (m_writeForTest) {
        return m_writeForTest(*m_file, data, size);
    }
    return m_file->write(data, size);
}

bool QsoRecorder::seekFile(qint64 position)
{
    if (m_seekForTest) {
        return m_seekForTest(*m_file, position);
    }
    return m_file->seek(position);
}

bool QsoRecorder::flushFile()
{
    if (m_flushForTest) {
        return m_flushForTest(*m_file);
    }
    return m_file->flush();
}

void QsoRecorder::queueWriteFailure(const QString& detail)
{
    // Called under m_writeMutex from the audio feed. Stop this and every
    // later feed before scheduling any owner-thread work; do not emit here.
    // Publish the pending finalization first: beginRecording() observes that
    // flag after it sees m_recording false, so it cannot replace m_file while
    // this feed still owns it.
    if (m_writeFailurePending.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    m_pendingWriteError = detail;
    m_recording.store(false, std::memory_order_release);
    const quint64 generation = m_recordingGeneration.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(this, [this, generation]() {
        finalizeWriteFailure(generation);
    }, Qt::QueuedConnection);
}

bool QsoRecorder::writeWavHeader()
{
    // Format is selected before the first published header. Later writes only
    // patch accepted byte lengths; no source transition can relabel this file.
    const std::optional<QByteArray> header = m_fileFormat->wavHeader(0);
    if (!header || writeFile(header->constData(), header->size()) != header->size()) {
        return false;
    }
    return flushFile();
}

bool QsoRecorder::patchWavHeader()
{
    if (!m_file || !m_file->isOpen()) {
        return false;
    }

    // Seek back and patch the two size fields in the WAV header
    if (!seekFile(4)) {
        return false;
    }
    quint32 riffSize = m_dataBytes + WAV_HEADER_SIZE - 8;
    char buf[4];
    qToLittleEndian<quint32>(riffSize, buf);
    if (writeFile(buf, 4) != 4) {
        return false;
    }

    if (!seekFile(40)) {
        return false;
    }
    qToLittleEndian<quint32>(m_dataBytes, buf);
    if (writeFile(buf, 4) != 4) {
        return false;
    }
    return flushFile();
}

// ── Playback ───────────────────────────────────────────────────────────────

std::optional<QByteArray> QsoRecorder::lastRecordingPcm(const QAudioFormat& format,
                                                        QString* error,
                                                        qint64 maxFrames,
                                                        bool prefixOnly) const
{
    if (m_lastRecordingPath.isEmpty()) {
        if (error) *error = tr("nothing has been recorded");
        return std::nullopt;
    }
    QFile file(m_lastRecordingPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return std::nullopt;
    }
    return prepareQsoWavPlayback(file, format, error, maxFrames, prefixOnly);
}

bool QsoRecorder::preparePlaybackPcm(const QAudioFormat& sinkFormat, QString& error)
{
    m_playPcm.clear();
    QFile file(m_lastRecordingPath);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        qCWarning(lcAudio) << "QsoRecorder: cannot open recording for playback:"
                           << file.errorString();
        return false;
    }
    std::optional<QByteArray> pcm = prepareQsoWavPlayback(file, sinkFormat, &error);
    if (!pcm) {
        qCWarning(lcAudio) << "QsoRecorder: cannot prepare recording for playback:" << error;
        return false;
    }
    m_playPcm = std::move(*pcm);
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
    if (dev.isNull()) {
        emit recordingError(tr("Cannot play this recording: no audio output device is available."));
        return;
    }

    // Negotiate the playback format via the shared factory (#3306, Phase 6b).
    // The recording is Int16, so prefer Int16 (no conversion on a normal device)
    // and fall back to Float for Float-only WASAPI mixers. The WAV helper
    // converts directly to that sink format (#3231). The factory supplies the per-OS
    // preferred rate (Win/Mac 48k to dodge the WASAPI 24k resampler artifacts
    // #2120; Linux native 24k) plus the 44.1k and preferredFormat fallbacks.
    // Previously QSO playback bailed on a Float-only device; now it works.
    // Walk with isFormatSupported (trusted), mirroring ClientPuduMonitor.
    QAudioFormat fmt;
    bool haveFormat = false;
    const QList<QAudioFormat> ladder = AudioDeviceNegotiator::formatLadder(
        dev, AudioFormatNegotiator::Direction::Output,
        AudioFormatNegotiator::ResamplerPolicy::PreservePan,
        AudioFormatNegotiator::hostTargetOs(),
        AudioFormatNegotiator::kInternalRate,
        /*bluetoothHfp=*/false, /*preferredRateOverride=*/0,
        AudioFormatNegotiator::FormatPreference::Int16First);
    // Preserve the existing stereo preference across the complete ladder.
    // A mono-only output can consume the helper's explicit stereo downmix.
    for (const int channels : {2, 1}) {
        for (const QAudioFormat& candidate : ladder) {
            QAudioFormat format = candidate;
            format.setChannelCount(channels);
            if (dev.isFormatSupported(format)) {
                fmt = format;
                haveFormat = true;
                break;
            }
        }
        if (haveFormat) {
            break;
        }
    }
    if (!haveFormat) {
        emit recordingError(tr("Cannot play this recording: the audio output has no supported format."));
        return;
    }
    startPlaybackWithFormat(dev, fmt);
}

void QsoRecorder::startPlaybackWithFormat(const QAudioDevice& device,
                                         const QAudioFormat& format)
{
    if (m_playing || m_lastRecordingPath.isEmpty()) {
        return;
    }
    QString preparationError;
    if (!preparePlaybackPcm(format, preparationError)) {
        // The local file has closed before observers may retry or destroy us.
        emit recordingError(tr("Cannot play this recording: %1").arg(preparationError));
        return;
    }

    m_playBuffer.close();
    m_playBuffer.setBuffer(&m_playPcm);
    if (!m_playBuffer.open(QIODevice::ReadOnly)) {
        emit recordingError(tr("Cannot open the recording playback buffer."));
        return;
    }

    // Synchronous sink failure must be retired BEFORE muting live RX. A
    // synchronous StoppedState callback is harmless while m_playing is false.
    const QAudio::Error error = startPlaybackSink(device, format);
    if (error != QAudio::NoError) {
        qCWarning(lcAudio) << "QsoRecorder: playback sink failed to start (error"
                           << error << ") — aborting, RX left live";
        releasePlaybackSink(false);
        m_playBuffer.close();
        emit recordingError(tr("Cannot start the recording audio output (error %1).")
                                .arg(static_cast<int>(error)));
        return;
    }

    m_playing = true;
    const quint64 generation = ++m_playbackGeneration;
    const QPointer<QsoRecorder> guard(this);
    emit muteRxRequested(true);
    // Direct signal observers may stop, replace, or destroy this playback.
    if (!guard || !m_playing || m_playbackGeneration != generation) {
        return;
    }
    emit playbackStarted();
}

QAudio::Error QsoRecorder::startPlaybackSink(const QAudioDevice& device,
                                            const QAudioFormat& format)
{
    if (m_startPlaybackSinkForTest) {
        return m_startPlaybackSinkForTest(m_playBuffer, format);
    }
    m_playSink = new QAudioSink(device, format, this);
    // 300 ms ring buffer: absorbs Windows WASAPI jitter (default ~40 ms starves
    // on event-loop hiccups and inserts silence).  Backend may clamp to its
    // period granularity; not an error if the effective size differs.
    m_playSink->setBufferSize(format.bytesForDuration(300'000));
    connect(m_playSink, &QAudioSink::stateChanged,
            this, &QsoRecorder::onPlaybackSinkState);
    m_playSink->start(&m_playBuffer);

    if (m_playSink->state() == QAudio::StoppedState) {
        return m_playSink->error();
    }
    return QAudio::NoError;
}

void QsoRecorder::releasePlaybackSink(bool stop)
{
    if (m_releasePlaybackSinkForTest) {
        m_releasePlaybackSinkForTest(stop);
        return;
    }
    if (m_playSink) {
        if (stop) {
            m_playSink->stop();
        }
        m_playSink->disconnect(this);
        // deleteLater(), NOT a direct delete, and it must stay that way:
        // onPlaybackSinkState() is a DIRECT connection from
        // QAudioSink::stateChanged, so a natural end-of-file arrives here with
        // the sink's own emission still on the stack. Destroying it there frees
        // the sender mid-emit; disconnect(this) severs the connection but does
        // not unwind that frame.
        //
        // The cost is that ~QsoRecorder cannot run the deferred delete, so the
        // sink falls to ~QObject instead -- after m_playBuffer and m_playPcm
        // have gone as members. That is safe because stop() above has already
        // halted the pull, and it stays safe only while this order holds.
        m_playSink->deleteLater();
        m_playSink = nullptr;
    }
}

void QsoRecorder::stopPlayback()
{
    if (!m_playing) return;
    m_playing = false;
    const quint64 generation = ++m_playbackGeneration;

    releasePlaybackSink(true);
    if (m_playBuffer.isOpen()) m_playBuffer.close();

    const QPointer<QsoRecorder> guard(this);
    emit muteRxRequested(false);
    if (!guard || m_playing || m_playbackGeneration != generation) {
        return;
    }
    emit playbackStopped();
}

void QsoRecorder::onPlaybackSinkState(QAudio::State state)
{
    if (state == QAudio::IdleState || state == QAudio::StoppedState) {
        stopPlayback();
    }
}

} // namespace AetherSDR
