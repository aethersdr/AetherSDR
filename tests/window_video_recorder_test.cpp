#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/WindowVideoRecorder.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QWidget>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QImage>
#include <QSignalSpy>
#include <QTimer>
#include <QElapsedTimer>
#include <QMediaFormat>
#include <QCursor>
#include <QEventLoop>
#include <QPoint>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    const QString a_ = (actual); const QString e_ = (expected); \
    if (a_ != e_) { \
        std::fprintf(stderr, "FAIL %s:%d  expected \"%s\", got \"%s\"\n", \
                     __FILE__, __LINE__, \
                     e_.toUtf8().constData(), a_.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  expected true: %s\n", \
                     __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    qputenv("AV_LOG_LEVEL", "error");
    TestSettingsProfile settingsProfile(QStringLiteral("aether-window-video-recorder-test"));
    if (!settingsProfile.isValid()) {
        return 1;
    }
    // Need QApplication instead of QCoreApplication because WindowVideoRecorder uses QWidget/QPixmap grab
    QApplication app(argc, argv);
    AppSettings::instance().load();
    QTemporaryDir tmp;
    AppSettings::instance().setValue("QsoRecordingDir", tmp.path());
    AppSettings::instance().save();
    EXPECT_TRUE(tmp.isValid());

    // Print supported media formats and codecs for diagnostics when requested
    if (qEnvironmentVariableIsSet("AETHER_TEST_VERBOSE")) {
        qWarning() << "=== SUPPORTED FILE FORMATS ===";
        for (auto format : QMediaFormat().supportedFileFormats(QMediaFormat::Encode)) {
            qWarning() << "Format:" << format;
        }
        qWarning() << "=== SUPPORTED VIDEO CODECS ===";
        for (auto codec : QMediaFormat().supportedVideoCodecs(QMediaFormat::Encode)) {
            qWarning() << "Video Codec:" << codec;
        }
        qWarning() << "=== SUPPORTED AUDIO CODECS ===";
        for (auto codec : QMediaFormat().supportedAudioCodecs(QMediaFormat::Encode)) {
            qWarning() << "Audio Codec:" << codec;
        }
        qWarning() << "==============================";
    }

    // Skip (rather than fail) when this environment cannot encode MP4/H.264/AAC.
    // The assertions below require a working encoder, which is an environment
    // property, not a property of the code under test.
    {
        QMediaFormat probe(QMediaFormat::MPEG4);
        probe.setVideoCodec(QMediaFormat::VideoCodec::H264);
        probe.setAudioCodec(QMediaFormat::AudioCodec::AAC);
        if (!probe.isSupported(QMediaFormat::Encode)) {
            std::fprintf(stderr,
                         "SKIP: Qt Multimedia cannot encode MP4/H.264/AAC in this environment\n");
            return 77;
        }
    }

    QWidget parentWidget;
    parentWidget.resize(640, 480);

    // Record ~1s of frames through the real capture path, wait for the
    // worker to finalize the file, then verify a non-empty .mp4 exists.
    {
        WindowVideoRecorder rec(&parentWidget);
        SliceModel slice(0);
        slice.setFrequency(7.125);
        slice.setMode("LSB");
        rec.setSlice(&slice);

        EXPECT_TRUE(!rec.isRecording());

        QSignalSpy startedSpy(&rec, &WindowVideoRecorder::recordingStarted);
        QSignalSpy stoppedSpy(&rec, &WindowVideoRecorder::recordingStopped);
        QSignalSpy errorSpy(&rec, &WindowVideoRecorder::recordingError);

        EXPECT_TRUE(rec.startRecording());
        EXPECT_TRUE(rec.isRecording());

        // Wait for recordingStarted (or error) before beginning the timed capture window
        QElapsedTimer t;
        t.start();
        while (startedSpy.isEmpty() && errorSpy.isEmpty() && t.elapsed() < 10000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }
        EXPECT_TRUE(!startedSpy.isEmpty());
        EXPECT_TRUE(errorSpy.isEmpty());

        // Let the capture timer produce ~2s of frames.
        t.restart();
        while (t.elapsed() < 2000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        rec.stopRecording();
        EXPECT_TRUE(!rec.isRecording());

        // Wait for the worker thread to flush the encoder and finalize the
        // file (signalled by recordingStopped).
        t.restart();
        while (stoppedSpy.isEmpty() && errorSpy.isEmpty() && t.elapsed() < 60000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        for (int i = 0; i < errorSpy.count(); ++i) {
            std::fprintf(stderr, "recordingError: %s\n",
                         errorSpy.at(i).at(0).toString().toUtf8().constData());
        }
        EXPECT_TRUE(errorSpy.isEmpty());
        EXPECT_TRUE(startedSpy.count() == 1);
        EXPECT_TRUE(stoppedSpy.count() == 1);

        QStringList files = QDir(tmp.path()).entryList(QStringList() << "*.mp4", QDir::Files);
        EXPECT_TRUE(!files.isEmpty());
        if (!files.isEmpty()) {
            QFileInfo info(tmp.path() + "/" + files.first());
            std::fprintf(stderr, "output file: %s size: %lld bytes\n",
                         info.fileName().toUtf8().constData(),
                         static_cast<long long>(info.size()));
            EXPECT_TRUE(info.size() > 1000);
        }
    }

    // Test Case 2: Test with a shown widget (fully realized window backing store & active screen)
    {
        QWidget visibleWidget;
        visibleWidget.resize(320, 240);
        visibleWidget.show();
        app.processEvents();

        // Move the cursor programmatically into the widget so the cursor-overlay code path is exercised.
        QCursor::setPos(visibleWidget.mapToGlobal(QPoint(50, 50)));
        app.processEvents();

        WindowVideoRecorder rec(&visibleWidget);
        SliceModel slice(0);
        slice.setFrequency(14.074);
        slice.setMode("FT8");
        rec.setSlice(&slice);

        QSignalSpy startedSpy(&rec, &WindowVideoRecorder::recordingStarted);
        QSignalSpy stoppedSpy(&rec, &WindowVideoRecorder::recordingStopped);
        QSignalSpy errorSpy(&rec, &WindowVideoRecorder::recordingError);

        EXPECT_TRUE(rec.startRecording());
        EXPECT_TRUE(rec.isRecording());

        // Wait for recordingStarted (or error) before beginning the timed capture window
        QElapsedTimer t;
        t.start();
        while (startedSpy.isEmpty() && errorSpy.isEmpty() && t.elapsed() < 10000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }
        EXPECT_TRUE(!startedSpy.isEmpty());
        EXPECT_TRUE(errorSpy.isEmpty());

        // Capture a few frames (~2s)
        t.restart();
        while (t.elapsed() < 2000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        rec.stopRecording();
        EXPECT_TRUE(!rec.isRecording());

        // Wait for worker to finalize
        t.restart();
        while (stoppedSpy.isEmpty() && errorSpy.isEmpty() && t.elapsed() < 30000) {
            app.processEvents(QEventLoop::AllEvents, 50);
        }

        EXPECT_TRUE(errorSpy.isEmpty());
        EXPECT_TRUE(startedSpy.count() == 1);
        EXPECT_TRUE(stoppedSpy.count() == 1);

        QStringList files = QDir(tmp.path()).entryList(QStringList() << "*14.074MHz_FT8*.mp4", QDir::Files);
        EXPECT_TRUE(!files.isEmpty());
        if (!files.isEmpty()) {
            QFileInfo info(tmp.path() + "/" + files.first());
            std::fprintf(stderr, "output visible file: %s size: %lld bytes\n",
                         info.fileName().toUtf8().constData(),
                         static_cast<long long>(info.size()));
            EXPECT_TRUE(info.size() > 1000);
        }
        visibleWidget.close();
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "Test failed with %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("PASS\n");
    return 0;
}

#else // QT_VERSION < QT_VERSION_CHECK(6, 8, 0)

#include <cstdio>
#include <QtGlobal>
int main()
{
    std::fprintf(stderr, "WARNING: Skipped WindowVideoRecorder tests because Qt version is < 6.8 (compiled with %s)\n", QT_VERSION_STR);
    return 77;
}

#endif

