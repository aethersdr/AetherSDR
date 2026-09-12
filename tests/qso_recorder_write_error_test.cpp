// Regression test for #5648 — recorder write failures must stop the audio
// feed, account only accepted bytes, and finalize/report on the owner thread.
//
// The seam below is deliberately post-open: filename allocation is covered by
// #5644, while these cases exercise QsoRecorder's production header, feed and
// finalization methods with deterministic QFile write/seek/flush outcomes.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/QsoRecorder.h"

#include <QCoreApplication>
#include <QEvent>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

using namespace AetherSDR;

namespace AetherSDR {

class QsoRecorderWriteErrorTestAccess {
public:
    static void setWriteHook(
        QsoRecorder& recorder,
        std::function<qint64(QFile&, const char*, qint64)> hook)
    {
        recorder.m_writeForTest = std::move(hook);
    }

    static void setSeekHook(QsoRecorder& recorder,
                            std::function<bool(QFile&, qint64)> hook)
    {
        recorder.m_seekForTest = std::move(hook);
    }

    static void setFlushHook(QsoRecorder& recorder, std::function<bool(QFile&)> hook)
    {
        recorder.m_flushForTest = std::move(hook);
    }

    static QFile* openFile(QsoRecorder& recorder)
    {
        return recorder.m_file;
    }
};

} // namespace AetherSDR

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
    const auto actual_ = (actual); const auto expected_ = (expected); \
    if (actual_ != expected_) { \
        std::fprintf(stderr, "FAIL %s:%d expected %lld, got %lld\n", \
                     __FILE__, __LINE__, static_cast<long long>(expected_), \
                     static_cast<long long>(actual_)); \
        ++g_failures; \
    } \
} while (0)

struct Events {
    int started{0};
    int stopped{0};
    int errors{0};
    QString stoppedPath;
};

void connectEvents(QsoRecorder& recorder, Events& events)
{
    QObject::connect(&recorder, &QsoRecorder::recordingStarted, &recorder,
                     [&events](const QString&) { ++events.started; });
    QObject::connect(&recorder, &QsoRecorder::recordingStopped, &recorder,
                     [&events](const QString& path, int) {
                         ++events.stopped;
                         events.stoppedPath = path;
                     });
    QObject::connect(&recorder, &QsoRecorder::recordingError, &recorder,
                     [&events](const QString&) { ++events.errors; });
}

void allowClientRecording()
{
    auto& settings = AppSettings::instance();
    settings.setValue(QStringLiteral("RecordingMode"), QStringLiteral("Client"));
    settings.setValue(QStringLiteral("PcAudioEnabled"), QStringLiteral("True"));
    settings.save();
}

void configure(QsoRecorder& recorder, const QString& directory)
{
    recorder.setRecordingDir(directory);
    recorder.setIncludeDate(false);
    recorder.setIncludeTime(false);
    recorder.setIncludeFrequency(false);
    recorder.setIncludeMode(false);
}

QByteArray rxFrame()
{
    QByteArray pcm(2 * static_cast<int>(sizeof(float)), Qt::Uninitialized);
    float* samples = reinterpret_cast<float*>(pcm.data());
    samples[0] = 0.5f;
    samples[1] = -0.5f;
    return pcm;
}

QByteArray txFrame()
{
    return QByteArray::fromHex("0100020003000400");
}

quint32 wavDataSize(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 44) {
        return 0;
    }
    const QByteArray header = file.read(44);
    return qFromLittleEndian<quint32>(header.constData() + 40);
}

quint32 wavRiffSize(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 44) {
        return 0;
    }
    const QByteArray header = file.read(44);
    return qFromLittleEndian<quint32>(header.constData() + 4);
}

void feedFromAudioThread(QsoRecorder& recorder, bool tx)
{
    std::thread feeder([&recorder, tx]() {
        if (tx) {
            recorder.feedTxAudio(txFrame());
        } else {
            recorder.feedRxAudio(rxFrame());
        }
    });
    feeder.join();
}

void deliverQueuedCalls()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents();
}

void testInitialHeaderFailures()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());

    {
        Events events;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        QsoRecorderWriteErrorTestAccess::setWriteHook(
            recorder, [](QFile& file, const char* data, qint64 size) {
                return file.write(data, size - 1);
            });

        recorder.startRecording();
        EXPECT_TRUE(!recorder.isRecording());
        EXPECT_EQ(events.started, 0);
        EXPECT_EQ(events.stopped, 0);
        EXPECT_EQ(events.errors, 1);
    }

    {
        Events events;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        QsoRecorderWriteErrorTestAccess::setFlushHook(
            recorder, [](QFile&) { return false; });

        recorder.startRecording();
        EXPECT_TRUE(!recorder.isRecording());
        EXPECT_EQ(events.started, 0);
        EXPECT_EQ(events.stopped, 0);
        EXPECT_EQ(events.errors, 1);
    }
}

void testFeedFailure(bool tx, qint64 result)
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    QThread* stoppedThread = nullptr;
    QThread* errorThread = nullptr;
    int writes = 0;
    qint64 actualAccepted = -1;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);
    QObject::connect(&recorder, &QsoRecorder::recordingStopped, &recorder,
                     [&stoppedThread](const QString&, int) {
                         stoppedThread = QThread::currentThread();
                     }, Qt::DirectConnection);
    QObject::connect(&recorder, &QsoRecorder::recordingError, &recorder,
                     [&errorThread](const QString&) {
                         errorThread = QThread::currentThread();
                     }, Qt::DirectConnection);

    QsoRecorderWriteErrorTestAccess::setWriteHook(
        recorder, [&writes, &actualAccepted, result](QFile& file, const char* data, qint64 size) {
            ++writes;
            if (writes != 2) {
                return file.write(data, size);
            }
            const qint64 accepted = std::clamp(result, qint64{0}, size);
            if (accepted > 0) {
                actualAccepted = file.write(data, accepted);
                return actualAccepted;
            }
            return result;
        });

    recorder.startRecording();
    EXPECT_TRUE(recorder.isRecording());
    if (tx) {
        recorder.onMoxChanged(true);
    }
    feedFromAudioThread(recorder, tx);

    // Incomplete writes stop the producer synchronously, while the error and
    // close remain queued to the recorder's owning thread.
    EXPECT_TRUE(!recorder.isRecording());
    EXPECT_EQ(events.errors, 0);
    EXPECT_EQ(events.stopped, 0);
    const qint64 requested = tx ? txFrame().size() : rxFrame().size() / 2;
    if (result > 0) {
        EXPECT_EQ(actualAccepted, result);
    }
    const int writesAfterFailure = writes;
    if (tx) {
        recorder.feedTxAudio(txFrame());
    } else {
        recorder.feedRxAudio(rxFrame());
    }
    EXPECT_EQ(writes, writesAfterFailure);

    // A pending failure owns the old handle; a restart before finalization is
    // ignored rather than replacing it under the feed thread.
    recorder.startRecording();
    EXPECT_TRUE(!recorder.isRecording());

    deliverQueuedCalls();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    EXPECT_TRUE(stoppedThread == recorder.thread());
    EXPECT_TRUE(errorThread == recorder.thread());
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    EXPECT_TRUE(!recorder.hasLastRecording());
    EXPECT_EQ(wavDataSize(events.stoppedPath), std::clamp(result, qint64{0}, requested));

    // recordingStopped still clears active consumers, but the failed file is
    // not published as the last playable recording. Do not open a platform
    // audio sink in this storage-only regression test.
    recorder.stopRecording();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);  // no duplicate zero-audio diagnostic
    if (recorder.isRecording()) {
        recorder.stopRecording();
    }
}

void testSuccessfulRecording(bool tx)
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);

    recorder.startRecording();
    EXPECT_TRUE(recorder.isRecording());
    if (tx) {
        recorder.onMoxChanged(true);
    }
    feedFromAudioThread(recorder, tx);
    recorder.stopRecording();

    const quint32 expectedData = tx ? txFrame().size() : rxFrame().size() / 2;
    EXPECT_EQ(events.started, 1);
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 0);
    EXPECT_EQ(wavDataSize(events.stoppedPath), expectedData);
    EXPECT_EQ(wavRiffSize(events.stoppedPath), expectedData + 36);
    EXPECT_EQ(QFile(events.stoppedPath).size(), expectedData + 44);
    EXPECT_TRUE(recorder.recordingFilePath() == events.stoppedPath);
}

void testActualReadOnlyFileFailure()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);

    recorder.startRecording();
    QFile* const file = QsoRecorderWriteErrorTestAccess::openFile(recorder);
    if (!file) {
        EXPECT_TRUE(false);
        return;
    }
    file->close();
    if (!file->open(QIODevice::ReadOnly)) {
        EXPECT_TRUE(false);
        return;
    }
    feedFromAudioThread(recorder, false);
    EXPECT_TRUE(!recorder.isRecording());
    EXPECT_EQ(events.errors, 0);
    deliverQueuedCalls();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    if (recorder.isRecording()) {
        recorder.stopRecording();
    }
}

void testFailedRunClearsPriorPlaybackPath()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    int writes = 0;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);

    recorder.startRecording();
    feedFromAudioThread(recorder, false);
    recorder.stopRecording();
    EXPECT_TRUE(recorder.hasLastRecording());

    QsoRecorderWriteErrorTestAccess::setWriteHook(
        recorder, [&writes](QFile& file, const char* data, qint64 size) {
            ++writes;
            return writes == 2 ? qint64{-1} : file.write(data, size);
        });
    recorder.startRecording();
    feedFromAudioThread(recorder, false);
    deliverQueuedCalls();

    EXPECT_TRUE(!recorder.hasLastRecording());
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    if (recorder.isRecording()) {
        recorder.stopRecording();
    }
}

void testFinalizationFailures()
{
    for (const qint64 failedSeek : {qint64{4}, qint64{40}}) {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        Events events;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        recorder.startRecording();
        recorder.feedRxAudio(rxFrame());
        QsoRecorderWriteErrorTestAccess::setSeekHook(
            recorder, [failedSeek](QFile& file, qint64 position) {
                return position == failedSeek ? false : file.seek(position);
            });
        recorder.stopRecording();
        EXPECT_EQ(events.stopped, 1);
        EXPECT_EQ(events.errors, 1);
        EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    }

    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        Events events;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        recorder.startRecording();
        recorder.feedRxAudio(rxFrame());
        QsoRecorderWriteErrorTestAccess::setWriteHook(
            recorder, [](QFile& file, const char* data, qint64 size) {
                return size == 4 ? qint64{-1} : file.write(data, size);
            });
        recorder.stopRecording();
        EXPECT_EQ(events.stopped, 1);
        EXPECT_EQ(events.errors, 1);
        EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    }

    // The first size-field patch can succeed while the second fails. Keep the
    // first real write so this is not merely the earlier-patch case repeated.
    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        Events events;
        int patchWrites = 0;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        recorder.startRecording();
        recorder.feedRxAudio(rxFrame());
        QsoRecorderWriteErrorTestAccess::setWriteHook(
            recorder, [&patchWrites](QFile& file, const char* data, qint64 size) {
                if (size == 4 && ++patchWrites == 2) {
                    return qint64{-1};
                }
                return file.write(data, size);
            });
        recorder.stopRecording();
        EXPECT_EQ(events.stopped, 1);
        EXPECT_EQ(events.errors, 1);
        EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    }

    {
        QTemporaryDir tmp;
        EXPECT_TRUE(tmp.isValid());
        Events events;
        int flushes = 0;
        QsoRecorder recorder;
        configure(recorder, tmp.path());
        connectEvents(recorder, events);
        QsoRecorderWriteErrorTestAccess::setFlushHook(
            recorder, [&flushes](QFile& file) {
                ++flushes;
                return flushes == 1 ? file.flush() : false;
            });
        recorder.startRecording();
        recorder.feedRxAudio(rxFrame());
        recorder.stopRecording();
        EXPECT_EQ(events.stopped, 1);
        EXPECT_EQ(events.errors, 1);
        EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    }
}

void testStaleQueuedFailureCannotTouchRestart()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    int writes = 0;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);
    QsoRecorderWriteErrorTestAccess::setWriteHook(
        recorder, [&writes](QFile& file, const char* data, qint64 size) {
            ++writes;
            return writes == 2 ? qint64{-1} : file.write(data, size);
        });
    recorder.startRecording();
    recorder.feedRxAudio(rxFrame());
    EXPECT_TRUE(!recorder.isRecording());

    // An explicit stop owns finalization before the queued audio-thread error
    // callback runs. The callback must then be harmless after a new start.
    recorder.stopRecording();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    QsoRecorderWriteErrorTestAccess::setWriteHook(recorder, {});
    recorder.startRecording();
    EXPECT_TRUE(recorder.isRecording());
    deliverQueuedCalls();
    EXPECT_TRUE(recorder.isRecording());
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    recorder.stopRecording();
    EXPECT_EQ(events.stopped, 2);
}

void testDestructionCancelsQueuedFailure()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    int writes = 0;
    auto recorder = std::make_unique<QsoRecorder>();
    configure(*recorder, tmp.path());
    connectEvents(*recorder, events);
    QsoRecorderWriteErrorTestAccess::setWriteHook(
        *recorder, [&writes](QFile& file, const char* data, qint64 size) {
            ++writes;
            return writes == 2 ? qint64{0} : file.write(data, size);
        });
    recorder->startRecording();
    recorder->feedRxAudio(rxFrame());
    recorder.reset();
    deliverQueuedCalls();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 0);  // teardown remains silent
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-qso-recorder-write-errors"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    allowClientRecording();

    testInitialHeaderFailures();
    for (const bool tx : {false, true}) {
        testSuccessfulRecording(tx);
        for (const qint64 result : {qint64{2}, qint64{0}, qint64{-1}}) {
            testFeedFailure(tx, result);
        }
    }
    testActualReadOnlyFileFailure();
    testFailedRunClearsPriorPlaybackPath();
    testFinalizationFailures();
    testStaleQueuedFailureCannotTouchRestart();
    testDestructionCancelsQueuedFailure();

    if (g_failures == 0) {
        std::printf("qso_recorder_write_error_test: all checks passed\n");
        return 0;
    }
    std::printf("qso_recorder_write_error_test: %d failure(s)\n", g_failures);
    return 1;
}
