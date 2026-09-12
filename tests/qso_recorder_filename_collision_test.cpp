// Regression test for #5640: QSO recordings claim filenames atomically, so a
// same-second start cannot truncate a populated WAV or a concurrent recording.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecorder.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

#define EXPECT_TRUE(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL %s:%d expected true: %s\n", \
                     __FILE__, __LINE__, #condition); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_EQ(actual, expected) do { \
    const QString actual_ = (actual); const QString expected_ = (expected); \
    if (actual_ != expected_) { \
        std::fprintf(stderr, "FAIL %s:%d expected %s, got %s\n", __FILE__, __LINE__, \
                     expected_.toUtf8().constData(), actual_.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

void allowClientRecording()
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("RecordingMode"), QStringLiteral("Client"));
    settings.setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));
    settings.save();
}

void disableFilenameFields(QsoRecorder& recorder)
{
    recorder.setIncludeDate(false);
    recorder.setIncludeTime(false);
    recorder.setIncludeFrequency(false);
    recorder.setIncludeMode(false);
}

QByteArray stereoFloatPcm(float sample)
{
    QByteArray pcm(2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    float* values = reinterpret_cast<float*>(pcm.data());
    values[0] = sample;
    values[1] = -sample;
    return pcm;
}

QByteArray expectedRecordedPcm(float sample)
{
    QByteArray pcm(2 * static_cast<int>(sizeof(qint16)), Qt::Uninitialized);
    qToLittleEndian<qint16>(static_cast<qint16>(sample * 32767.0f), pcm.data());
    qToLittleEndian<qint16>(static_cast<qint16>(-sample * 32767.0f),
                            pcm.data() + sizeof(qint16));
    return pcm;
}

QString startAndCapture(QsoRecorder& recorder, float sample)
{
    recorder.startRecording();
    const QString path = recorder.recordingFilePath();
    EXPECT_TRUE(recorder.isRecording());
    recorder.feedRxAudio(stereoFloatPcm(sample));
    recorder.stopRecording();
    return path;
}

bool writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(contents) == contents.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

int runWorker(const QString& directory, float sample)
{
    allowClientRecording();
    std::fputs("READY\n", stdout);
    std::fflush(stdout);

    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly) || input.readLine().trimmed() != "go") {
        return 1;
    }

    QsoRecorder recorder;
    recorder.setRecordingDir(directory);
    disableFilenameFields(recorder);
    recorder.startRecording();
    if (!recorder.isRecording()) {
        return 1;
    }
    recorder.feedRxAudio(stereoFloatPcm(sample));
    const QString path = recorder.recordingFilePath();
    std::fprintf(stdout, "CLAIM %s\n", path.toUtf8().constData());
    std::fflush(stdout);
    if (input.readLine().trimmed() != "finish") {
        return 1;
    }
    recorder.stopRecording();
    std::fprintf(stdout, "DONE %s\n", path.toUtf8().constData());
    return 0;
}

bool workerSucceeded(QProcess& worker)
{
    return (worker.state() == QProcess::NotRunning || worker.waitForFinished(10000))
           && worker.exitStatus() == QProcess::NormalExit
           && worker.exitCode() == 0;
}

QString readWorkerLine(QProcess& worker, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        if (worker.canReadLine()) {
            return QString::fromUtf8(worker.readLine()).trimmed();
        }
        const int remainingMs = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (!worker.waitForReadyRead(qMin(remainingMs, 1000)) && worker.state() == QProcess::NotRunning) {
            break;
        }
    }
    return {};
}

bool sendWorkerCommand(QProcess& worker, const QByteArray& command)
{
    return worker.write(command) == command.size()
           && (worker.waitForBytesWritten(1000) || worker.bytesToWrite() == 0);
}

void stopWorker(QProcess& worker)
{
    if (worker.state() != QProcess::NotRunning) {
        worker.kill();
        worker.waitForFinished(1000);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const bool worker = argc == 4
                        && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--worker");
    TestSettingsProfile settingsProfile(worker
        ? QStringLiteral("aether-qso-recorder-collision-worker")
        : QStringLiteral("aether-qso-recorder-filename-collision"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    if (worker) {
        return runWorker(QString::fromLocal8Bit(argv[2]),
                         QString::fromLocal8Bit(argv[3]).toFloat());
    }
    allowClientRecording();

    // A populated WAV is created through the production audio feed. Reusing
    // the same recorder must leave its exact bytes intact, even with a sentinel
    // occupying the next candidate.
    {
        QTemporaryDir directory;
        EXPECT_TRUE(directory.isValid());
        QsoRecorder recorder;
        recorder.setRecordingDir(directory.path());
        disableFilenameFields(recorder);
        const QString original = startAndCapture(recorder, 0.25f);
        const QByteArray originalBytes = readFile(original);
        EXPECT_TRUE(originalBytes.size() > 44);
        const QString sentinel = directory.filePath(QStringLiteral("QSO_1.wav"));
        EXPECT_TRUE(writeFile(sentinel, QByteArrayLiteral("do not overwrite")));

        const QString second = startAndCapture(recorder, 0.50f);
        const QString third = startAndCapture(recorder, 0.75f);
        EXPECT_EQ(QFileInfo(original).fileName(), QStringLiteral("QSO.wav"));
        EXPECT_EQ(QFileInfo(second).fileName(), QStringLiteral("QSO_2.wav"));
        EXPECT_EQ(QFileInfo(third).fileName(), QStringLiteral("QSO_3.wav"));
        EXPECT_TRUE(readFile(original) == originalBytes);
        EXPECT_TRUE(readFile(sentinel) == QByteArrayLiteral("do not overwrite"));
    }

    // Keep the complete filename stem intact: decimal frequency components and
    // sanitized portable callsigns both precede the collision suffix.
    {
        QTemporaryDir directory;
        EXPECT_TRUE(directory.isValid());
        const QString existing = directory.filePath(
            QStringLiteral("14.250MHz_USB_CALL_P.wav"));
        EXPECT_TRUE(writeFile(existing, QByteArrayLiteral("populated WAV")));

        SliceModel slice(0);
        slice.setFrequency(14.250);
        slice.setMode(QStringLiteral("USB"));
        QsoRecorder recorder;
        recorder.setRecordingDir(directory.path());
        recorder.setIncludeDate(false);
        recorder.setIncludeTime(false);
        recorder.setCallsign(QStringLiteral("CALL/P"));
        recorder.setSlice(&slice);
        const QString path = startAndCapture(recorder, 0.25f);
        EXPECT_EQ(QFileInfo(path).fileName(), QStringLiteral("14.250MHz_USB_CALL_P_1.wav"));
        EXPECT_TRUE(readFile(existing) == QByteArrayLiteral("populated WAV"));
    }

#ifndef Q_OS_WIN
    // QFileInfo::exists() is false for dangling links. They still occupy the
    // name, so the recorder must reserve the next candidate instead.
    {
        QTemporaryDir directory;
        EXPECT_TRUE(directory.isValid());
        const QString dangling = directory.filePath(QStringLiteral("QSO.wav"));
        QFile missingTarget(directory.filePath(QStringLiteral("missing-target")));
        EXPECT_TRUE(missingTarget.link(dangling));
        QsoRecorder recorder;
        recorder.setRecordingDir(directory.path());
        disableFilenameFields(recorder);
        const QString path = startAndCapture(recorder, 0.25f);
        EXPECT_EQ(QFileInfo(path).fileName(), QStringLiteral("QSO_1.wav"));
    }
#endif

    // Exercise the default date/time form. Each retry uses an empty temporary
    // directory, avoiding a second attempt inheriting an earlier suffix after a
    // wall-clock boundary.
    {
        bool observedSameSecondCollision = false;
        constexpr int kAttempts = 3;
        for (int attempt = 0; attempt < kAttempts && !observedSameSecondCollision; ++attempt) {
            QTemporaryDir directory;
            EXPECT_TRUE(directory.isValid());
            QsoRecorder first;
            first.setRecordingDir(directory.path());
            const QString firstPath = startAndCapture(first, 0.25f);
            QsoRecorder second;
            second.setRecordingDir(directory.path());
            const QString secondPath = startAndCapture(second, 0.50f);
            const QFileInfo firstInfo(firstPath);
            const QFileInfo secondInfo(secondPath);
            observedSameSecondCollision = secondInfo.fileName()
                == firstInfo.completeBaseName() + QStringLiteral("_1.wav");
        }
        EXPECT_TRUE(observedSameSecondCollision);
    }

    // The workers wait on a stdin barrier, then race for the same local-file
    // candidates. Both announce an owned, still-open file before either can
    // finalize, proving that separate recordings can coexist with both files open.
    {
        QTemporaryDir directory;
        EXPECT_TRUE(directory.isValid());
        QProcess first;
        QProcess second;
        const QString executable = QCoreApplication::applicationFilePath();
        first.start(executable, {QStringLiteral("--worker"), directory.path(), QStringLiteral("0.25")});
        second.start(executable, {QStringLiteral("--worker"), directory.path(), QStringLiteral("0.50")});
        const bool started = first.waitForStarted() && second.waitForStarted();
        EXPECT_TRUE(started);
        const bool ready = started
                           && readWorkerLine(first, 10000) == QStringLiteral("READY")
                           && readWorkerLine(second, 10000) == QStringLiteral("READY");
        EXPECT_TRUE(ready);
        bool firstReleased = false;
        bool secondReleased = false;
        if (ready) {
            firstReleased = sendWorkerCommand(first, "go\n");
            secondReleased = sendWorkerCommand(second, "go\n");
        }
        const bool released = ready && firstReleased && secondReleased;
        EXPECT_TRUE(released);

        QString firstPath;
        QString secondPath;
        bool claimed = false;
        if (released) {
            const QString firstClaim = readWorkerLine(first, 10000);
            const QString secondClaim = readWorkerLine(second, 10000);
            if (firstClaim.startsWith(QStringLiteral("CLAIM "))
                && secondClaim.startsWith(QStringLiteral("CLAIM "))) {
                firstPath = firstClaim.sliced(6);
                secondPath = secondClaim.sliced(6);
                claimed = !firstPath.isEmpty() && !secondPath.isEmpty() && firstPath != secondPath;
            }
        }
        EXPECT_TRUE(claimed);

        bool firstFinished = false;
        bool secondFinished = false;
        if (claimed) {
            firstFinished = sendWorkerCommand(first, "finish\n");
            secondFinished = sendWorkerCommand(second, "finish\n");
        }
        const bool finished = claimed && firstFinished && secondFinished;
        EXPECT_TRUE(finished);
        if (finished) {
            EXPECT_EQ(readWorkerLine(first, 10000), QStringLiteral("DONE ") + firstPath);
            EXPECT_EQ(readWorkerLine(second, 10000), QStringLiteral("DONE ") + secondPath);
            EXPECT_TRUE(workerSucceeded(first));
            EXPECT_TRUE(workerSucceeded(second));
            const QByteArray firstBytes = readFile(firstPath);
            const QByteArray secondBytes = readFile(secondPath);
            EXPECT_TRUE(firstBytes.mid(44) == expectedRecordedPcm(0.25f));
            EXPECT_TRUE(secondBytes.mid(44) == expectedRecordedPcm(0.50f));
        }
        stopWorker(first);
        stopWorker(second);
    }

    // This reaches QFile::open() itself. An invalid component cannot be fixed
    // by suffixing, so it emits one error and leaves no recording state behind.
    {
        QTemporaryDir directory;
        EXPECT_TRUE(directory.isValid());
        QsoRecorder recorder;
        recorder.setRecordingDir(directory.path());
        disableFilenameFields(recorder);
        recorder.setCallsign(QString(300, QLatin1Char('A')));
        int errors = 0;
        int started = 0;
        QObject::connect(&recorder, &QsoRecorder::recordingError,
                         &recorder, [&](const QString&) { ++errors; });
        QObject::connect(&recorder, &QsoRecorder::recordingStarted,
                         &recorder, [&](const QString&) { ++started; });
        recorder.startRecording();
        EXPECT_TRUE(!recorder.isRecording());
        EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
        EXPECT_TRUE(errors == 1);
        EXPECT_TRUE(started == 0);
    }

    if (g_failures == 0) {
        std::printf("qso_recorder_filename_collision_test: all checks passed\n");
        return 0;
    }
    std::printf("qso_recorder_filename_collision_test: %d failure(s)\n", g_failures);
    return 1;
}
