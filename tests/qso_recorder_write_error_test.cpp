// Regression test for #5648 — recorder write failures must stop the audio
// feed, account only accepted bytes, and finalize/report on the owner thread.
//
// The seam below is deliberately post-open: filename allocation is covered by
// #5644, while these cases exercise QsoRecorder's production header, feed and
// finalization methods with deterministic QFile write/seek/flush outcomes.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/PcmFrame.h"
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
#include <optional>
#include <thread>

#ifdef Q_OS_WIN
#include <io.h>
#include <qt_windows.h>
#else
#include <unistd.h>
#endif

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

enum class TailBoundary { Mox, Cw, Stop };
constexpr qint64 kDelayedTailBytes = 7 * 2 * 4; // 7 frames at 24k -> 14 stereo PCM16 frames at 48k

QByteArray delayedPcm()
{
    const qint16 samples[] = {
        8192, -16384, 8192, -16384, 8192, -16384, 8192, -16384,
        8192, -16384, 8192, -16384, 8192, -16384};
    return QByteArray(reinterpret_cast<const char*>(samples), sizeof(samples));
}

// Select 48k from live RX metadata, then feed a short fixed24 voice/CW segment
// on a joined audio thread. No data reaches QFile until the finite tail drains.
QString startWithDelayedTail(QsoRecorder& recorder, PcmProducer& producer,
                             TailBoundary boundary)
{
    EXPECT_TRUE(producer.start(PcmPurpose::Speaker, -1, {48000, PcmLayout::Stereo}));
    const std::optional<PcmFrame> observed = producer.produce({0.0f, 0.0f});
    EXPECT_TRUE(observed.has_value());
    if (!observed) {
        return {};
    }
    recorder.feedRxFrame(*observed);
    recorder.startRecording();
    EXPECT_TRUE(recorder.isRecording());
    const QString path = recorder.recordingFilePath();
    if (boundary == TailBoundary::Cw) {
        recorder.setCwOverActive(true);
    } else {
        recorder.onMoxChanged(true);
    }
    std::thread feeder([&recorder, boundary]() {
        if (boundary == TailBoundary::Cw) {
            recorder.feedCwAudio(delayedPcm());
        } else {
            recorder.feedTxAudio(delayedPcm());
        }
    });
    feeder.join();
    EXPECT_TRUE(recorder.isRecording());
    EXPECT_EQ(QFile(path).size(), 44);
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray header = file.read(44);
    EXPECT_EQ(header.size(), 44);
    if (header.size() == 44) {
        EXPECT_EQ(qFromLittleEndian<quint32>(header.constData() + 24), 48000);
    }
    return path;
}

void drainAtBoundary(QsoRecorder& recorder, TailBoundary boundary)
{
    if (boundary == TailBoundary::Mox) {
        recorder.onMoxChanged(false);
    } else if (boundary == TailBoundary::Cw) {
        recorder.setCwOverActive(false);
    } else {
        recorder.stopRecording();
    }
}

void testUnequalRateTailWriteFailure(TailBoundary boundary, qint64 result)
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    PcmProducer producer;
    int tailWrites = 0;
    qint64 requestedTailBytes = 0;
    qint64 acceptedTailBytes = 0;
    QThread* writeThread = nullptr;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);
    const QString path = startWithDelayedTail(recorder, producer, boundary);
    QsoRecorderWriteErrorTestAccess::setWriteHook(
        recorder, [&](QFile& file, const char* data, qint64 size) {
            if (file.pos() < 44) {
                return file.write(data, size); // Preserve the real final size patches.
            }
            ++tailWrites;
            requestedTailBytes = size;
            writeThread = QThread::currentThread();
            if (result > 0) {
                acceptedTailBytes = file.write(data, std::min(result, size));
                return acceptedTailBytes;
            }
            return result;
        });

    drainAtBoundary(recorder, boundary);
    EXPECT_EQ(tailWrites, 1);
    EXPECT_EQ(requestedTailBytes, kDelayedTailBytes);
    EXPECT_EQ(acceptedTailBytes, std::max(result, qint64{0}));
    EXPECT_TRUE(writeThread == recorder.thread());
    EXPECT_TRUE(!recorder.isRecording());
    if (boundary != TailBoundary::Stop) {
        // A transition failure queues finalization, just like a feed failure.
        EXPECT_EQ(events.stopped, 0);
        EXPECT_EQ(events.errors, 0);
        recorder.startRecording();
        EXPECT_TRUE(!recorder.isRecording());
    }
    recorder.feedTxAudio(delayedPcm());
    recorder.feedCwAudio(delayedPcm());
    EXPECT_EQ(tailWrites, 1);
    deliverQueuedCalls();
    EXPECT_EQ(events.started, 1);
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    EXPECT_TRUE(events.stoppedPath == path);
    EXPECT_TRUE(!recorder.hasLastRecording());
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    // A two-byte partial tail remains two accepted bytes in a failed file;
    // do not silently round it to a frame or advertise it for playback.
    EXPECT_EQ(wavDataSize(path), acceptedTailBytes);
    EXPECT_EQ(wavRiffSize(path), acceptedTailBytes + 36);
    EXPECT_EQ(QFile(path).size(), acceptedTailBytes + 44);
    recorder.stopRecording();
    deliverQueuedCalls();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
}

void testFlushFailureAfterUnequalRateTail(TailBoundary boundary)
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    PcmProducer producer;
    qint64 acceptedTailBytes = 0;
    int finalFlushes = 0;
    bool tailWrittenBeforeFlush = false;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);
    const QString path = startWithDelayedTail(recorder, producer, boundary);
    QsoRecorderWriteErrorTestAccess::setWriteHook(
        recorder, [&acceptedTailBytes](QFile& file, const char* data, qint64 size) {
            const bool tail = file.pos() >= 44;
            const qint64 accepted = file.write(data, size);
            if (tail) {
                acceptedTailBytes += std::max(accepted, qint64{0});
            }
            return accepted;
        });
    QsoRecorderWriteErrorTestAccess::setFlushHook(
        recorder, [&](QFile&) {
            ++finalFlushes;
            tailWrittenBeforeFlush = acceptedTailBytes == kDelayedTailBytes;
            return false;
        });
    drainAtBoundary(recorder, boundary);
    EXPECT_EQ(acceptedTailBytes, kDelayedTailBytes);
    if (boundary != TailBoundary::Stop) {
        // MOX/CW drain the segment; only finalization flushes the WAV file.
        EXPECT_TRUE(recorder.isRecording());
        EXPECT_EQ(finalFlushes, 0);
        EXPECT_EQ(events.stopped, 0);
        EXPECT_EQ(events.errors, 0);
        recorder.stopRecording();
    }
    EXPECT_TRUE(tailWrittenBeforeFlush);
    EXPECT_EQ(finalFlushes, 1);
    EXPECT_TRUE(!recorder.isRecording());
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    EXPECT_TRUE(!recorder.hasLastRecording());
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    EXPECT_EQ(wavDataSize(path), kDelayedTailBytes);
    EXPECT_EQ(wavRiffSize(path), kDelayedTailBytes + 36);
    EXPECT_EQ(QFile(path).size(), kDelayedTailBytes + 44);
    recorder.stopRecording();
    deliverQueuedCalls();
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
}

void testDestructionDrainsAfterFeedJoin(bool failTail)
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    PcmProducer producer;
    qint64 acceptedTailBytes = 0;
    auto recorder = std::make_unique<QsoRecorder>();
    configure(*recorder, tmp.path());
    connectEvents(*recorder, events);
    const QString path = startWithDelayedTail(*recorder, producer, TailBoundary::Stop);
    QsoRecorderWriteErrorTestAccess::setWriteHook(
        *recorder, [&](QFile& file, const char* data, qint64 size) {
            if (file.pos() < 44) {
                return file.write(data, size);
            }
            acceptedTailBytes = file.write(data, failTail ? std::min(qint64{2}, size) : size);
            return acceptedTailBytes;
        });
    // The only feeder was joined by startWithDelayedTail. Destruction is on
    // the owner thread, never concurrent with a caller using the recorder.
    recorder.reset();
    deliverQueuedCalls();
    EXPECT_EQ(acceptedTailBytes, failTail ? qint64{2} : kDelayedTailBytes);
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 0); // Tail failure remains silent during teardown.
    EXPECT_TRUE(events.stoppedPath == path);
    EXPECT_EQ(wavDataSize(path), acceptedTailBytes);
    EXPECT_EQ(wavRiffSize(path), acceptedTailBytes + 36);
    EXPECT_EQ(QFile(path).size(), acceptedTailBytes + 44);
}

void testCloseFailureAfterSuccessfulFlush()
{
    QTemporaryDir tmp;
    EXPECT_TRUE(tmp.isValid());
    Events events;
    int flushes = 0;
    bool finalFlushSucceeded = false;
    QsoRecorder recorder;
    configure(recorder, tmp.path());
    connectEvents(recorder, events);
    QsoRecorderWriteErrorTestAccess::setFlushHook(
        recorder, [&flushes, &finalFlushSucceeded](QFile& file) {
            const bool flushed = file.flush();
            if (++flushes == 2 && flushed) {
                finalFlushSucceeded = true;
                // Invalidate only this test-owned handle after the header and
                // final flush succeeded. QFile::close() must now report its
                // native close error; no real storage failure is required.
#ifdef Q_OS_WIN
                // Leave the CRT descriptor valid for Qt's _close cleanup;
                // invalidating the descriptor itself invokes MSVC's invalid-
                // parameter handler when Qt closes it a second time.
                const HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(file.handle()));
                EXPECT_TRUE(::CloseHandle(handle));
#else
                EXPECT_EQ(::close(file.handle()), 0);
#endif
            }
            return flushed;
        });

    recorder.startRecording();
    recorder.feedRxAudio(rxFrame());
    recorder.stopRecording();
    EXPECT_TRUE(finalFlushSucceeded);
    EXPECT_TRUE(!recorder.isRecording());
    EXPECT_EQ(events.stopped, 1);
    EXPECT_EQ(events.errors, 1);
    EXPECT_TRUE(!recorder.hasLastRecording());
    EXPECT_TRUE(recorder.recordingFilePath().isEmpty());
    EXPECT_EQ(wavDataSize(events.stoppedPath), rxFrame().size() / 2);
    recorder.stopRecording();
    EXPECT_EQ(events.errors, 1);
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
    for (const TailBoundary boundary : {TailBoundary::Mox, TailBoundary::Cw, TailBoundary::Stop}) {
        for (const qint64 result : {qint64{2}, qint64{0}, qint64{-1}}) {
            testUnequalRateTailWriteFailure(boundary, result);
        }
        testFlushFailureAfterUnequalRateTail(boundary);
    }
    testDestructionDrainsAfterFeedJoin(false);
    testDestructionDrainsAfterFeedJoin(true);
    testCloseFailureAfterSuccessfulFlush();
    testStaleQueuedFailureCannotTouchRestart();
    testDestructionCancelsQueuedFailure();

    if (g_failures == 0) {
        std::printf("qso_recorder_write_error_test: all checks passed\n");
        return 0;
    }
    std::printf("qso_recorder_write_error_test: %d failure(s)\n", g_failures);
    return 1;
}
