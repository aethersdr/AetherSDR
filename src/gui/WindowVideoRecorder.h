#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QWidget>
#include <QTimer>
#include <QImage>
#include <QElapsedTimer>
#include <QPointer>
#include <QList>

#include <memory>

namespace AetherSDR {
class SliceModel;
class INativeVideoWriter;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)

class QMediaCaptureSession;
class QMediaRecorder;
class QVideoFrameInput;
class QAudioBufferInput;

namespace AetherSDR {

class WindowVideoRecorder : public QObject {
    Q_OBJECT

public:
    explicit WindowVideoRecorder(QWidget* mainWindow, QObject* parent = nullptr);
    ~WindowVideoRecorder() override;

    bool isRecording() const;
    // True from successful start until finalize completes (includes stop-in-flight).
    bool isSessionActive() const;

    void setSlice(SliceModel* slice);

public slots:
    // Returns true when a recording session was armed. False if already active,
    // finalize is in flight, or open failed immediately (recordingError emitted).
    bool startRecording();
    void stopRecording();

    void feedRxAudio(const QByteArray& pcm);
    void feedTxAudio(const QByteArray& int16Stereo);
    void onMoxChanged(bool mox);

signals:
    void recordingStarted(const QString& filePath);
    void recordingStopped(const QString& filePath, int durationSecs);
    void recordingError(const QString& message);

private slots:
    void captureFrame();
    bool sendSilentAudio();

private:
    QString buildFilename() const;

    QWidget* m_mainWindow{nullptr};
    QPointer<SliceModel> m_slice;

    bool m_recording{false};
    bool m_stopping{false};
    bool m_readyToSend{false};
    QString m_recordingDir;
    QString m_currentFilePath;
    QDateTime m_startTime;
    double m_freqMhz{0.0};
    QString m_mode;

    QTimer* m_captureTimer{nullptr};
    QElapsedTimer m_recordTimer;
    int m_videoWidth{1280};
    int m_videoHeight{720};
    QImage m_frameBuffer;
    QImage m_scaledBuffer;

    QMediaCaptureSession* m_session{nullptr};
    // Not parented to the session: it does not take ownership, and these are
    // torn down and rebuilt per recording. unique_ptr keeps that explicit.
    std::unique_ptr<QVideoFrameInput> m_videoFrameInput;
    std::unique_ptr<QAudioBufferInput> m_audioBufferInput;
    QMediaRecorder* m_recorder{nullptr};
    int m_frameCount{0};
    bool m_transmitting{false};
    bool m_hasRealAudio{false};
    quint64 m_audioSamplesSent{0};
    bool sendAudioData(const QByteArray& int16Stereo);
    void initQtMultimediaPipeline();
    void cleanupQtInputs();
    void finalizeStop();
    // Pins both media clocks to the instant the encoder actually starts
    // accepting frames. See WindowVideoRecorder.cpp for why.
    void anchorEncoderClock();

    // Latches once the encoder proves it is live so the video clock is
    // anchored exactly once per session.
    bool m_encoderAnchored{false};
    int m_readySignalCount{0};

    bool m_useFallback{false};

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    std::unique_ptr<INativeVideoWriter> m_fallbackWriter;
#endif
    QList<QPointer<QWidget>> m_rhiWidgetsCache;
};

} // namespace AetherSDR

#else // QT_VERSION < QT_VERSION_CHECK(6, 8, 0)

namespace AetherSDR {

class WindowVideoRecorder : public QObject {
    Q_OBJECT

public:
    explicit WindowVideoRecorder(QWidget* /*mainWindow*/, QObject* parent = nullptr) : QObject(parent) {}
    ~WindowVideoRecorder() override = default;

    bool isRecording() const { return false; }
    bool isSessionActive() const { return false; }

    void setSlice(SliceModel* /*slice*/) {}

public slots:
    bool startRecording() { return false; }
    void stopRecording() {}

    void feedRxAudio(const QByteArray& /*pcm*/) {}
    void feedTxAudio(const QByteArray& /*int16Stereo*/) {}
    void onMoxChanged(bool /*mox*/) {}

signals:
    void recordingStarted(const QString& filePath);
    void recordingStopped(const QString& filePath, int durationSecs);
    void recordingError(const QString& message);
};

} // namespace AetherSDR

#endif
