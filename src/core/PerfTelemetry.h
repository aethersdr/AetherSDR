#pragma once

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <atomic>

namespace AetherSDR {

class PerfTelemetry final {
public:
    enum class FrameKind {
        Panadapter,
        Waterfall
    };

    enum class GpuUploadKind {
        WaterfallFull,
        WaterfallIncremental,
        Overlay
    };

    static PerfTelemetry& instance();
    static qint64 nowNs();

    bool enabled() const;

    void recordUdpBatch(int datagrams, qint64 bytes, double drainMs);
    void recordStreamPacket(FrameKind kind, bool sequenceError);
    void recordFrameRestart(FrameKind kind);
    void recordFrameAge(FrameKind kind, double ageMs);

    void recordPanFrame();
    void recordWaterfallNativeRows(int rows);
    void recordWaterfallFallbackRows(int rows);
    void recordWaterfallVisibleRows(int rows = 1);
    void recordWaterfallRebuild();
    void setWaterfallLineDurationMs(int ms);

    void recordPanCenterCommand();
    void recordPanUpdate(double updateMs);
    void recordWaterfallUpdate(double updateMs);
    void recordRender(double renderMs);
    void recordGpuUpload(GpuUploadKind kind);

    void recordUiHeartbeat();
    void setDragActive(bool active);
    void recordMouseMoveGap(double gapMs);
    void recordInputEvent(const char* kind, double durationMs);

    void recordSHistoryProcessed(double durationMs);
    void recordSHistorySkipped();

private:
    PerfTelemetry() = default;

    struct Window {
        qint64 udpBytes{0};
        int udpBatches{0};
        int udpDatagrams{0};
        int udpMaxDatagramsPerBatch{0};
        double udpDrainMaxMs{0.0};

        int fftPackets{0};
        int fftErrors{0};
        int waterfallPackets{0};
        int waterfallErrors{0};
        int fftFrameRestarts{0};
        int waterfallFrameRestarts{0};

        int panFrames{0};
        int panCenterCommands{0};
        int waterfallFrames{0};
        int waterfallNativeRows{0};
        int waterfallFallbackRows{0};
        int waterfallVisibleRows{0};
        int waterfallRebuilds{0};

        int gpuUploads{0};
        int gpuWaterfallFullUploads{0};
        int gpuWaterfallIncrementalUploads{0};
        int gpuOverlayUploads{0};

        int shistoryProcessed{0};
        int shistorySkipped{0};

        bool dragSeen{false};
        double uiLagMaxMs{0.0};
        double mouseMoveGapMaxMs{0.0};

        QVector<double> panAgeMs;
        QVector<double> waterfallAgeMs;
        QVector<double> panUpdateMs;
        QVector<double> waterfallUpdateMs;
        QVector<double> renderMs;
        QVector<double> inputMs;
        QVector<double> shistoryMs;
    };

    bool beginRecord(qint64 nowNs);
    void maybeLogSummary(qint64 nowNs);
    void resetWindowLocked(qint64 nowNs);

    static double percentile95(QVector<double> values);
    static double nsToMs(qint64 ns);
    static QString msValue(double value);
    static QString pctValue(int errors, int packets);
    static QString keyValue(const QString& key, const QString& value);
    static QString keyValue(const QString& key, int value);
    static QString keyValue(const QString& key, qint64 value);
    static QString keyValueMs(const QString& key, double value);
    static void logStall(const QStringList& fields);

    mutable QMutex m_mutex;
    Window m_window;
    qint64 m_windowStartNs{0};
    qint64 m_lastHeartbeatNs{0};
    std::atomic_bool m_wasEnabled{false};
    std::atomic_bool m_dragActive{false};
    std::atomic<int> m_waterfallLineDurationMs{0};
};

} // namespace AetherSDR
