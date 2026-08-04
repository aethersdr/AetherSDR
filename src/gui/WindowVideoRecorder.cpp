#include "WindowVideoRecorder.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)

#include "core/AppSettings.h"
#include "models/SliceModel.h"

#include <QDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QDateTime>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QUrl>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QCursor>

#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QVideoFrameInput>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QMediaFormat>
#include <QAudioBufferInput>
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QLibrary>
#include <QRhiWidget>
#include <algorithm>
#include <cstdint>
#include <utility>

#include "recording/INativeVideoWriter.h"
#include "recording/MediaBackendProbe.h"
#if defined(Q_OS_WIN)
#include "recording/WmfVideoWriter.h"
#elif defined(Q_OS_MAC)
#include "recording/AvfVideoWriter.h"
#endif

namespace {
Q_LOGGING_CATEGORY(lcWindowVideoRecorder, "aether.windowvideorecorder")

constexpr int kAudioSampleRate = 24000;
constexpr int kAudioChannels = 2;
constexpr int kAudioBytesPerSample = static_cast<int>(sizeof(int16_t));
constexpr int kAudioBytesPerFrame = kAudioChannels * kAudioBytesPerSample;
constexpr int kSilenceChunkMs = 40; // 40 ms silent chunk (matching 25 fps frame step)

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
// True when Qt Multimedia can drive custom QVideoFrameInput/QAudioBufferInput
// capture into an MP4/H.264/AAC encode. Construction of QVideoFrameInput is not
// a capability probe (operator new never returns null).
bool qtMultimediaEncoderUsable()
{
#if defined(Q_OS_MAC)
    // Custom frame-input capture is non-functional on our macOS Qt builds;
    // always use the AVFoundation writer.
    return false;
#else
    QMediaFormat fmt(QMediaFormat::MPEG4);
    fmt.setVideoCodec(QMediaFormat::VideoCodec::H264);
    fmt.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    if (!fmt.isSupported(QMediaFormat::Encode)) {
        qCWarning(lcWindowVideoRecorder) << "QMediaFormat MP4/H264/AAC encode not supported by Qt Multimedia";
        return false;
    }

    // Custom frame/buffer inputs are only reliable with the FFmpeg backend.
    // An explicit operator override wins; otherwise ask whether the plugin is
    // actually installed. Reading QT_MEDIA_BACKEND alone would be circular —
    // main() writes it — which is what made the native writer unreachable.
    const QString backend = qEnvironmentVariable("QT_MEDIA_BACKEND").toLower();
    if (!backend.isEmpty() && backend != QLatin1String("ffmpeg")) {
        qCInfo(lcWindowVideoRecorder) << "QT_MEDIA_BACKEND is not ffmpeg; using native writer. backend=" << backend;
        return false;
    }
    if (!AetherSDR::ffmpegMediaBackendAvailable()) {
        qCInfo(lcWindowVideoRecorder) << "Qt FFmpeg multimedia plugin not present; using native writer.";
        return false;
    }
    return true;
#endif
}
#endif

void setFFmpegLogLevel(int level)
{
    using SetLevelFunc = void (*)(int);
    static SetLevelFunc setLevel = nullptr;
    static bool resolved = false;
    // Keep the loader alive for process lifetime. Resolving a symbol from a
    // temporary QLibrary and then destroying it can unload the DSO and leave
    // setLevel dangling (common on Linux where only libavutil.so.NN exists).
    static QLibrary avutilLib;

    if (!resolved) {
        resolved = true;

        auto tryResolve = [](QLibrary& lib) -> SetLevelFunc {
            if (!lib.isLoaded() && !lib.load()) {
                return nullptr;
            }
            return reinterpret_cast<SetLevelFunc>(lib.resolve("av_log_set_level"));
        };

        // 1. Already mapped into the process (Qt FFmpeg plugin / static link).
        setLevel = reinterpret_cast<SetLevelFunc>(
            QLibrary::resolve(QStringLiteral("avutil"), "av_log_set_level"));
        if (!setLevel) {
            setLevel = reinterpret_cast<SetLevelFunc>(
                QLibrary::resolve(QStringLiteral("libavutil"), "av_log_set_level"));
        }

#if defined(Q_OS_LINUX)
        // 2. Unversioned soname.
        if (!setLevel) {
            avutilLib.setFileName(QStringLiteral("avutil"));
            setLevel = tryResolve(avutilLib);
        }
        if (!setLevel) {
            avutilLib.setFileName(QStringLiteral("libavutil"));
            setLevel = tryResolve(avutilLib);
        }

        // 3. Versioned sonames (libavutil.so.56 … .60).
        for (int ver = 56; ver <= 60 && !setLevel; ++ver) {
            avutilLib.setFileNameAndVersion(QStringLiteral("avutil"), ver);
            setLevel = tryResolve(avutilLib);
            if (!setLevel) {
                avutilLib.setFileNameAndVersion(QStringLiteral("libavutil"), ver);
                setLevel = tryResolve(avutilLib);
            }
        }
#endif
    }

    if (setLevel) {
        setLevel(level);
    }
}
}

namespace AetherSDR {

WindowVideoRecorder::WindowVideoRecorder(QWidget* mainWindow, QObject* parent)
    : QObject(parent)
    , m_mainWindow(mainWindow)
{
    m_recordingDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                     + "/AetherSDR/Recordings";

    auto& s = AppSettings::instance();
    m_recordingDir = s.value("QsoRecordingDir", m_recordingDir).toString();

#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    m_useFallback = !qtMultimediaEncoderUsable();
#endif

    m_captureTimer = new QTimer(this);
    connect(m_captureTimer, &QTimer::timeout, this, &WindowVideoRecorder::captureFrame);

    if (!m_useFallback) {
        initQtMultimediaPipeline();
    }
}

void WindowVideoRecorder::initQtMultimediaPipeline()
{
    if (m_session || m_recorder) {
        return;
    }

    // Initialize native Qt Multimedia pipeline
    m_session = new QMediaCaptureSession(this);
    m_recorder = new QMediaRecorder(this);
    m_session->setRecorder(m_recorder);

    // Connect state tracking and finish notification signals
    connect(m_recorder, &QMediaRecorder::recorderStateChanged, this, [this](QMediaRecorder::RecorderState state) {
        if (state == QMediaRecorder::RecordingState) {
            // Stop requested before the async RecordingState arrived — abort immediately.
            if (m_stopping || !m_recording) {
                m_recorder->stop();
                return;
            }
            // The media clocks are NOT started here: RecordingState arrives
            // well before the encoder actually accepts frames (~0.9 s on the
            // Linux FFmpeg backend). anchorEncoderClock() runs on the first
            // readyToSendVideoFrame instead.
            QString finalPath = m_recorder->actualLocation().toLocalFile();
            if (finalPath.isEmpty()) {
                finalPath = m_currentFilePath;
            }
            int vBitrate = m_recorder->videoBitRate();
            int aBitrate = m_recorder->audioBitRate();
            QMediaFormat mediaFmt = m_recorder->mediaFormat();
            QString vCodecName = QMediaFormat::videoCodecName(mediaFmt.videoCodec());
            QString aCodecName = QMediaFormat::audioCodecName(mediaFmt.audioCodec());

            qCInfo(lcWindowVideoRecorder) << "Recording started."
                                          << "Path:" << finalPath
                                          << "Resolution:" << m_videoWidth << "x" << m_videoHeight
                                          << "Video Codec:" << vCodecName
                                          << "Audio Codec:" << aCodecName
                                          << "Video Bitrate:" << (vBitrate > 0 ? QString::number(vBitrate) : QStringLiteral("Auto"))
                                          << "Audio Bitrate:" << (aBitrate > 0 ? QString::number(aBitrate) : QStringLiteral("Auto"))
                                          << "Quality:" << m_recorder->quality();
            emit recordingStarted(finalPath);
        } else if (state == QMediaRecorder::StoppedState) {
            // Only finalize sessions we armed/are stopping; ignore idle noise.
            if (m_stopping || m_recording) {
                QString finalPath = m_recorder->actualLocation().toLocalFile();
                if (finalPath.isEmpty()) {
                    finalPath = m_currentFilePath;
                }
                QFileInfo fi(finalPath);
                qint64 sizeBytes = fi.size();
                int durationSecs = static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()));
                qCInfo(lcWindowVideoRecorder) << "Recording stopped."
                                              << "Path:" << finalPath
                                              << "Size:" << sizeBytes << "bytes"
                                              << "Duration:" << durationSecs << "seconds";
                // Prefer the recorder's actual path for the stopped signal.
                m_currentFilePath = finalPath;
                finalizeStop();
            }
        }
    });

    connect(m_recorder, &QMediaRecorder::errorOccurred, this, [this](QMediaRecorder::Error error, const QString& errorString) {
        if (m_useFallback) {
            return;
        }
        qCWarning(lcWindowVideoRecorder) << "Qt Recorder error occurred:" << error << "-" << errorString;
        emit recordingError(errorString);
        // Tear down the session so UI "not recording" matches encoder state.
        if (m_recording && !m_stopping) {
            stopRecording();
        }
    });
}

void WindowVideoRecorder::cleanupQtInputs()
{
    if (m_videoFrameInput) {
        if (m_session) {
            m_session->setVideoFrameInput(nullptr);
        }
        m_videoFrameInput.reset();
    }
    if (m_audioBufferInput) {
        if (m_session) {
            m_session->setAudioBufferInput(nullptr);
        }
        m_audioBufferInput.reset();
    }
}

void WindowVideoRecorder::anchorEncoderClock()
{
    // The video clock restarts here rather than at record(): the FFmpeg backend
    // needs ~0.9 s to accept its second frame and discards everything offered
    // in the meantime. Timing video from record() therefore stamped the first
    // surviving frame at ~0.9 s while the audio the backend did accept was
    // concatenated from 0, freezing the opening frame and leaving the two
    // tracks offset by the encoder start-up time for the whole file.
    // m_audioSamplesSent is deliberately NOT reset: it already counts only
    // buffers the backend accepted, so it is the true audio write position.
    m_encoderAnchored = true;
    m_hasRealAudio = false;
    m_frameCount = 0;
    m_recordTimer.start();
}

void WindowVideoRecorder::finalizeStop()
{
    if (m_captureTimer) {
        m_captureTimer->stop();
    }
    m_encoderAnchored = false;
    cleanupQtInputs();

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (m_fallbackWriter) {
        m_fallbackWriter->close();
        m_fallbackWriter.reset();
    }
    m_useFallback = !qtMultimediaEncoderUsable();
#endif

    // m_recording is cleared eagerly in stopRecording() so isRecording() is
    // false immediately; m_stopping covers the finalize window and gates the emit.
    const bool shouldEmit = m_stopping || m_recording;
    const QString path = m_currentFilePath;
    const int durationSecs = m_startTime.isValid()
        ? static_cast<int>(m_startTime.secsTo(QDateTime::currentDateTimeUtc()))
        : 0;

    m_recording = false;
    m_stopping = false;
    m_readyToSend = false;
    m_hasRealAudio = false;
    m_transmitting = false;

    if (shouldEmit) {
        emit recordingStopped(path, durationSecs);
    }
}

WindowVideoRecorder::~WindowVideoRecorder()
{
    // Disconnect/block signals before teardown so destruction is strictly side-effect-free
    blockSignals(true);
    if (m_recorder) {
        m_recorder->disconnect();
    }

    if (m_recording || m_stopping) {
        // Best-effort sync teardown on destroy (no further UI signals needed).
        m_stopping = true;
        if (m_captureTimer) {
            m_captureTimer->stop();
        }
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
        if (m_fallbackWriter) {
            m_fallbackWriter->close();
            m_fallbackWriter.reset();
        }
#endif
        if (m_recorder && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
            m_recorder->stop();
        }
        cleanupQtInputs();
        m_recording = false;
        m_stopping = false;
    }
}

bool WindowVideoRecorder::isRecording() const
{
    return m_recording;
}

bool WindowVideoRecorder::isSessionActive() const
{
    return m_recording || m_stopping;
}

void WindowVideoRecorder::setSlice(SliceModel* slice)
{
    m_slice = slice;
}

bool WindowVideoRecorder::startRecording()
{
    qCDebug(lcWindowVideoRecorder) << "startRecording() called. m_recording:" << m_recording
                                   << "m_stopping:" << m_stopping;
    if (m_recording || m_stopping) {
        return false;
    }

    m_frameCount = 0;

    m_rhiWidgetsCache.clear();
    if (m_mainWindow) {
        for (auto* rhi : m_mainWindow->findChildren<QRhiWidget*>()) {
            m_rhiWidgetsCache.append(rhi);
        }
    }

    if (m_slice) {
        m_freqMhz = m_slice->frequency();
        m_mode = m_slice->mode();
    } else {
        m_freqMhz = 0.0;
        m_mode.clear();
    }

    m_startTime = QDateTime::currentDateTimeUtc();

    QDir dir(m_recordingDir);
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qCWarning(lcWindowVideoRecorder) << "Failed to create recording directory:" << m_recordingDir;
            emit recordingError("Cannot create recording directory: " + m_recordingDir);
            return false;
        }
    }

    QString filePath = m_recordingDir + "/" + buildFilename();
    qCDebug(lcWindowVideoRecorder) << "Output path set to:" << filePath;

    // Target resolution comes from the window geometry alone — rendering a
    // throwaway frame here cost a full CPU render plus a GPU framebuffer grab
    // and its pixels were never encoded.
    if (m_mainWindow) {
        const QSize windowSize = m_mainWindow->size();
        m_videoWidth = (windowSize.width() / 16) * 16;
        m_videoHeight = (windowSize.height() / 16) * 16;
    }

    if (m_videoWidth < 64) {
        m_videoWidth = 1280;
    }
    if (m_videoHeight < 64) {
        m_videoHeight = 720;
    }
    m_videoWidth = (m_videoWidth / 16) * 16;
    m_videoHeight = (m_videoHeight / 16) * 16;

    qCDebug(lcWindowVideoRecorder) << "Video resolution:" << m_videoWidth << "x" << m_videoHeight;

    m_currentFilePath = filePath;

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    m_useFallback = !qtMultimediaEncoderUsable();

    if (m_useFallback) {
        qCInfo(lcWindowVideoRecorder) << "Using native platform video writer (Qt Multimedia encode path unavailable).";
#if defined(Q_OS_WIN)
        m_fallbackWriter = std::make_unique<WmfVideoWriter>();
#elif defined(Q_OS_MAC)
        m_fallbackWriter = std::make_unique<AvfVideoWriter>();
#endif
        if (m_fallbackWriter && m_fallbackWriter->open(filePath, m_videoWidth, m_videoHeight, 25)) {
            // Mark session active immediately so stopRecording() cannot no-op.
            m_recording = true;
            m_stopping = false;
            m_recordTimer.start();
            m_readyToSend = true;
            m_encoderAnchored = true;
            m_audioSamplesSent = 0;
            m_hasRealAudio = false;
            m_transmitting = false;
            emit recordingStarted(filePath);
            m_captureTimer->start(40);
            return true;
        }
        m_fallbackWriter.reset();
        emit recordingError(QStringLiteral("Failed to open native video recording engine."));
        return false;
    }
#endif

    initQtMultimediaPipeline();
    if (!m_recorder || !m_session) {
        emit recordingError(QStringLiteral("Failed to initialize Qt video recording engine."));
        return false;
    }

    if (ffmpegMediaBackendAvailable()) {
        if (lcWindowVideoRecorder().isDebugEnabled()) {
            qputenv("AV_LOG_LEVEL", "info");
            setFFmpegLogLevel(32); // AV_LOG_INFO
        } else {
            qputenv("AV_LOG_LEVEL", "error");
            setFFmpegLogLevel(16); // AV_LOG_ERROR
        }
    }

    cleanupQtInputs();

    m_videoFrameInput = std::make_unique<QVideoFrameInput>();
    m_session->setVideoFrameInput(m_videoFrameInput.get());

    // Re-bind recorder to session to force rediscovery of the new video frame input
    m_session->setRecorder(nullptr);
    m_session->setRecorder(m_recorder);

    m_readyToSend = false;
    m_encoderAnchored = false;
    m_readySignalCount = 0;
    connect(m_videoFrameInput.get(), &QVideoFrameInput::readyToSendVideoFrame, this, [this]() {
        m_readyToSend = true;
        // The first signal is emitted as soon as the input is attached, long
        // before the encoder can take a second frame. Only the signal that
        // follows the priming frame proves the encoder is actually live, so
        // that is where the media clocks are anchored.
        if (!m_encoderAnchored && ++m_readySignalCount >= 2) {
            anchorEncoderClock();
        }
    });

    m_audioSamplesSent = 0;
    m_hasRealAudio = false;
    m_transmitting = false;

    m_audioBufferInput = std::make_unique<QAudioBufferInput>();
    m_session->setAudioBufferInput(m_audioBufferInput.get());

    // Modern Qt6 style: use QMediaFormat to set MP4 file format and H.264 video codec
    QMediaFormat mediaFormat;
    mediaFormat.setFileFormat(QMediaFormat::MPEG4);
    mediaFormat.setVideoCodec(QMediaFormat::VideoCodec::H264);
    mediaFormat.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    m_recorder->setMediaFormat(mediaFormat);
    m_recorder->setVideoResolution(QSize(m_videoWidth, m_videoHeight));
    m_recorder->setVideoFrameRate(25.0);

    m_recorder->setOutputLocation(QUrl::fromLocalFile(filePath));
    m_recorder->setQuality(QMediaRecorder::NormalQuality);

    // Arm the session before the async RecordingState so an immediate Stop works.
    m_recording = true;
    m_stopping = false;
    // The media clock stays unstarted until anchorEncoderClock(): the encoder
    // does not accept frames for a while after record() and any audio fed in
    // the meantime is silently discarded, which desynchronises the file.
    m_recordTimer.invalidate();

    qCDebug(lcWindowVideoRecorder) << "Calling m_recorder->record(). Current state:" << m_recorder->recorderState();
    m_recorder->record();

    // Start video capture at 25 fps (40 ms) on Main Thread
    m_captureTimer->start(40);
    qCDebug(lcWindowVideoRecorder) << "Started capture timer (40ms interval)";
    return true;
}

void WindowVideoRecorder::stopRecording()
{
    qCDebug(lcWindowVideoRecorder) << "stopRecording() called. m_recording:" << m_recording
                                   << "m_stopping:" << m_stopping;
    // Tear down any armed session, even if RecordingState has not arrived yet.
    if (!m_recording || m_stopping) {
        return;
    }

    // Eagerly clear m_recording so isRecording() is false immediately (UI/tests),
    // while m_stopping keeps finalize/recordingStopped coherent for close-on-stop.
    m_stopping = true;
    m_recording = false;
    if (m_captureTimer) {
        m_captureTimer->stop();
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (m_useFallback) {
        finalizeStop();
        return;
    }
#endif

    cleanupQtInputs();

    if (m_recorder && m_recorder->recorderState() != QMediaRecorder::StoppedState) {
        m_recorder->stop();
        qCDebug(lcWindowVideoRecorder) << "m_recorder->stop() executed; waiting for StoppedState";
        // finalizeStop() runs from recorderStateChanged(StoppedState).
        // If stop is synchronous (or never left StoppedState), finalize now so
        // callers always observe recordingStopped.
        if (m_stopping && m_recorder->recorderState() == QMediaRecorder::StoppedState) {
            finalizeStop();
        } else if (m_stopping) {
            // Watchdog fallback: if backend stalls or fails to emit StoppedState within 3s,
            // force finalizeStop() so session is not permanently stuck in finalizing state.
            QTimer::singleShot(3000, this, [this]() {
                if (m_stopping) {
                    qCWarning(lcWindowVideoRecorder) << "Stop timeout (3s) elapsed waiting for StoppedState; forcing finalizeStop()";
                    finalizeStop();
                }
            });
        }
        return;
    }

    // Never entered RecordingState / already stopped — finish synchronously so
    // close-while-recording and UI toggles always observe recordingStopped.
    finalizeStop();
}

void WindowVideoRecorder::captureFrame()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (!m_recording || !m_mainWindow || (!m_useFallback && (!m_videoFrameInput || !m_audioBufferInput))) {
        return;
    }
#else
    if (!m_recording || !m_mainWindow || !m_videoFrameInput || !m_audioBufferInput) {
        return;
    }
#endif

    const qint64 ptsUs = m_recordTimer.isValid()
        ? (m_recordTimer.nsecsElapsed() / 1000)
        : (m_frameCount * 40000);

    // Keep audio stream synchronized with video timestamp to prevent FFmpeg muxer queue deadlocks
    while (!m_hasRealAudio && static_cast<qint64>((m_audioSamplesSent * 1000000) / kAudioSampleRate) < ptsUs + (kSilenceChunkMs * 1000)) {
        if (!sendSilentAudio()) {
            break;
        }
    }

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (!m_useFallback && !m_readyToSend) {
        return;
    }
#else
    if (!m_readyToSend) {
        return;
    }
#endif


    QSize mainSize = m_mainWindow->size();
    if (mainSize.width() <= 0 || mainSize.height() <= 0) {
        return;
    }

    if (m_frameBuffer.size() != mainSize || m_frameBuffer.format() != QImage::Format_ARGB32) {
        m_frameBuffer = QImage(mainSize, QImage::Format_ARGB32);
    }
    m_frameBuffer.fill(Qt::black);
    m_mainWindow->render(&m_frameBuffer);

    // Composite GPU-accelerated QRhiWidgets (such as SpectrumWidget) and cursor on top
    QPoint globalPos = QCursor::pos();
    QPoint localPos = m_mainWindow->mapFromGlobal(globalPos);
    const bool drawCursor = m_mainWindow->rect().contains(localPos);

    if (!m_rhiWidgetsCache.isEmpty() || drawCursor) {
        QPainter painter(&m_frameBuffer);

        for (const auto& widget : m_rhiWidgetsCache) {
            if (widget && widget->isVisible()) {
                if (auto* rhi = qobject_cast<QRhiWidget*>(widget.data())) {
                    QImage childImg = rhi->grabFramebuffer();
                    if (!childImg.isNull()) {
                        QPoint pos = rhi->mapTo(m_mainWindow, QPoint(0, 0));
                        painter.drawImage(pos, childImg);
                    }

                    // Render standard child widgets of the QRhiWidget on top
                    for (QObject* obj : rhi->children()) {
                        if (auto* w = qobject_cast<QWidget*>(obj)) {
                            if (w->isVisible() && !w->inherits("QRhiWidget")) {
                                QPoint pos = w->mapTo(m_mainWindow, QPoint(0, 0));
                                w->render(&painter, pos);
                            }
                        }
                    }
                }
            }
        }

        // Overlay mouse cursor if it is currently inside the window bounds
        if (drawCursor) {
            painter.setRenderHint(QPainter::Antialiasing);

            // Classic black cursor with a crisp white outline for high contrast
            painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::black);

            QPainterPath path;
            path.moveTo(localPos);
            path.lineTo(localPos + QPoint(0, 17));
            path.lineTo(localPos + QPoint(4, 13));
            path.lineTo(localPos + QPoint(8, 20));
            path.lineTo(localPos + QPoint(11, 18));
            path.lineTo(localPos + QPoint(7, 12));
            path.lineTo(localPos + QPoint(12, 12));
            path.closeSubpath();

            painter.drawPath(path);
        }
    }

    // Scale to macroblock-aligned target resolution if needed
    const QSize targetSize(m_videoWidth, m_videoHeight);
    if (m_frameBuffer.size() != targetSize) {
        m_scaledBuffer = m_frameBuffer.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    const QImage& frameToEncode = (m_frameBuffer.size() == targetSize) ? m_frameBuffer : m_scaledBuffer;

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (m_useFallback && m_fallbackWriter) {
        if (static_cast<qint64>((m_audioSamplesSent * 1000000) / kAudioSampleRate) < ptsUs) {
            sendSilentAudio();
        }
        bool success = m_fallbackWriter->writeVideoFrame(frameToEncode, ptsUs);
        if (!success) {
            emit recordingError(QStringLiteral("Failed to write video frame to native container"));
            stopRecording();
            return;
        }
        m_frameCount++;
        return;
    }
#endif

    // Construct QVideoFrame directly from the scaled image
    QVideoFrame frame(frameToEncode);

    if (frame.isValid()) {
        frame.setStartTime(ptsUs);
        frame.setEndTime(ptsUs + 40000); // 40ms frame duration at 25 fps (microseconds)

        if (static_cast<qint64>((m_audioSamplesSent * 1000000) / kAudioSampleRate) < ptsUs) {
            sendSilentAudio();
        }

        bool sent = m_videoFrameInput->sendVideoFrame(frame);
        if (sent) {
            m_readyToSend = false;
        } else {
            m_readyToSend = true;
            emit recordingError(QStringLiteral("Failed to encode video frame"));
            stopRecording();
            return;
        }
        m_frameCount++;
        qCDebug(lcWindowVideoRecorder) << "Recorder State:" << m_recorder->recorderState()
                                         << "Frame validity:" << frame.isValid()
                                         << "Pixel Format:" << frame.pixelFormat()
                                         << "Width:" << frame.width() << "Height:" << frame.height()
                                         << "StartTime:" << frame.startTime()
                                         << "Sent result:" << sent;
    } else {
        qCWarning(lcWindowVideoRecorder) << "Constructed frame is invalid!";
    }
}

namespace {

QByteArray float32ToInt16(const QByteArray& pcm)
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

} // namespace

bool WindowVideoRecorder::sendSilentAudio()
{
    constexpr int samplesPerChunk = (kAudioSampleRate * kSilenceChunkMs) / 1000;
    constexpr int bufferSize = samplesPerChunk * kAudioBytesPerFrame;
    static const QByteArray kSilentBytes(bufferSize, 0);
    return sendAudioData(kSilentBytes);
}

void WindowVideoRecorder::feedRxAudio(const QByteArray& pcm)
{
    if (!m_recording || m_transmitting) {
        return;
    }
    QByteArray converted = float32ToInt16(pcm);
    if (sendAudioData(converted)) {
        m_hasRealAudio = true;
    }
}

void WindowVideoRecorder::feedTxAudio(const QByteArray& int16Stereo)
{
    if (!m_recording || !m_transmitting) {
        return;
    }
    if (sendAudioData(int16Stereo)) {
        m_hasRealAudio = true;
    }
}

void WindowVideoRecorder::onMoxChanged(bool mox)
{
    m_transmitting = mox;
}

bool WindowVideoRecorder::sendAudioData(const QByteArray& int16Stereo)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (!m_recording || (!m_useFallback && !m_audioBufferInput)) {
        return false;
    }
#else
    if (!m_recording || !m_audioBufferInput) {
        return false;
    }
#endif

    // Audio must keep flowing even before the encoder is live, otherwise the
    // FFmpeg muxer stalls waiting on the audio queue and never becomes ready.
    // Pre-ready buffers are rejected by the backend and simply not counted.

    // NOTE: m_hasRealAudio is deliberately NOT set here. sendSilentAudio()
    // routes through this function too, so setting it here would let the very
    // first synthetic silence chunk latch the flag and permanently disable the
    // silence priming it gates.

    QByteArray pcmData = int16Stereo;
    const int remainder = pcmData.size() % kAudioBytesPerFrame;
    if (remainder != 0) {
        pcmData.truncate(pcmData.size() - remainder);
    }
    if (pcmData.isEmpty()) {
        return false;
    }

    static const QAudioFormat kFormat = []() {
        QAudioFormat fmt;
        fmt.setSampleRate(kAudioSampleRate);
        fmt.setChannelCount(kAudioChannels);
        fmt.setSampleFormat(QAudioFormat::Int16);
        return fmt;
    }();

    qint64 ptsUs = (m_audioSamplesSent * 1000000) / kAudioSampleRate;
    int numFrames = pcmData.size() / kAudioBytesPerFrame;

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    if (m_useFallback && m_fallbackWriter) {
        bool ok = m_fallbackWriter->writeAudioSamples(pcmData, ptsUs);
        if (ok) {
            m_audioSamplesSent += numFrames;
        }
        return ok;
    }
#endif

    QAudioBuffer buffer(pcmData, kFormat, ptsUs);
    // Only accepted samples advance the clock: the FFmpeg backend concatenates
    // the buffers it takes and ignores their start times, so counting a
    // rejected buffer would shift every later timestamp ahead of the audio
    // actually written.
    if (buffer.isValid() && m_audioBufferInput->sendAudioBuffer(buffer)) {
        m_audioSamplesSent += numFrames;
        return true;
    }
    return false;
}

QString WindowVideoRecorder::buildFilename() const
{
    QStringList parts;

    parts << m_startTime.toString("yyyy-MM-dd");
    parts << m_startTime.toString("HHmmss") + "Z";

    if (m_freqMhz > 0.0) {
        parts << QString::number(m_freqMhz, 'f', 3) + "MHz";
    }

    if (!m_mode.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("[^A-Z0-9]"));
        QString sanitizedMode = m_mode.toUpper();
        sanitizedMode.replace(re, QStringLiteral("_"));
        parts << sanitizedMode;
    }

    if (parts.isEmpty()) {
        parts << "WindowRecord";
    }

    return parts.join("_") + ".mp4";
}

} // namespace AetherSDR

#endif // QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
