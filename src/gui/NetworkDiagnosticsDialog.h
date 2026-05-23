#pragma once

#include "PersistentDialog.h"
#include "core/PanadapterStream.h"

#include <QComboBox>
#include <QFile>
#include <QHeaderView>
#include <QLabel>
#include <QObject>
#include <QSet>
#include <QTableWidget>
#include <QTimer>
#include <QVector>

class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QLineEdit;

namespace AetherSDR {

class RadioModel;
class AudioEngine;
class TimeSeriesGraphWidget;
class TciServer;

struct NetworkDiagnosticsSample {
    qint64 timestampMs{0};
    int    rttMs{0};
    int    audioGapMs{0};
    int    audioJitterMs{0};
    double rxKbps{0.0};
    double txKbps{0.0};
    double audioKbps{0.0};
    double fftKbps{0.0};
    double waterfallKbps{0.0};
    double meterKbps{0.0};
    double daxKbps{0.0};
    double packetLossPct{0.0};
    double audioLossPct{0.0};
    double fftLossPct{0.0};
    double waterfallLossPct{0.0};
    double meterLossPct{0.0};
    double daxLossPct{0.0};
    double audioBufferMs{0.0};
    double underrunsPerSecond{0.0};
    double audioFeedRateHz{0.0};
    double audioFeedDeficitMs{0.0};
    double audioLatePacketsPerSecond{0.0};
    qint64 audioLatePackets{0};
    qint64 audioPacketGaps{0};
    qint64 audioLastPacketAgeMs{0};
    quint16 audioPacketClassCode{0};
    int audioStreamCount{0};
};

class NetworkDiagnosticsHistory : public QObject {
public:
    explicit NetworkDiagnosticsHistory(RadioModel* model, AudioEngine* audio, QObject* parent = nullptr);

    const QVector<NetworkDiagnosticsSample>& samples() const { return m_samples; }
    NetworkDiagnosticsSample latestSample() const;

private:
    void sampleNow();
    void pruneSamples(qint64 nowMs);

    RadioModel* m_model{nullptr};
    AudioEngine* m_audio{nullptr};
    QTimer m_sampleTimer;
    QVector<NetworkDiagnosticsSample> m_samples;
    qint64 m_lastRxBytes{0};
    qint64 m_lastTxBytes{0};
    qint64 m_lastSampleMs{0};
    quint64 m_lastAudioUnderrunCount{0};
    qint64 m_lastAudioLatePackets{0};
    qint64 m_lastCatBytes[PanadapterStream::CatCount]{};
};

class NetworkDiagnosticsDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit NetworkDiagnosticsDialog(RadioModel* model,
                                      AudioEngine* audio,
                                      NetworkDiagnosticsHistory* history,
                                      TciServer* tci = nullptr,
                                      QWidget* parent = nullptr);

private:
    struct LogLine {
        QString text;
        QString category;
    };

    void refresh();
    void updateCharts();
    QWidget* buildLogsTab();
    QWidget* buildTciTab();
    void     refreshTciClientTable();
    void     appendTciMessage(const QString& direction, const QString& text);
    void     onTciSaveLog();
    void     onTciLogContextMenu(const QPoint& pos);
    void     tciSuppress(const QString& cmd);
    void     refreshTciSuppressLabel();
    void initializeLogTail();
    bool reopenLogFile(bool keepExistingLines);
    void appendNewLogData();
    void appendLogText(const QString& text);
    void addLogLine(const QString& line);
    void rebuildLogView();
    bool logLineVisible(const LogLine& line) const;
    QString logCategoryFromLine(const QString& line) const;
    void setLogFollowLive(bool on);
    void setAllLogCategoriesVisible(bool visible);
    int selectedRangeSeconds() const;

    RadioModel* m_model;
    AudioEngine* m_audio;
    NetworkDiagnosticsHistory* m_history{nullptr};
    TciServer*  m_tci{nullptr};
    QTableWidget* m_tciClientTable{nullptr};
    QLabel*       m_tciClientSummary{nullptr};
    QTableWidget*   m_tciLogTable{nullptr};
    QLabel*         m_tciSuppressLabel{nullptr};
    QPushButton*    m_tciPauseBtn{nullptr};
    bool            m_tciMonitorPaused{false};
    QSet<QString>   m_tciSuppressed;   // command prefixes muted by the user
    QTimer      m_refreshTimer;
    QTimer      m_logRefreshTimer;
    QComboBox*  m_rangeCombo{nullptr};

    QLabel* m_statusLabel;
    QLabel* m_targetIpLabel;
    QLabel* m_sourcePathLabel;
    QLabel* m_tcpEndpointLabel;
    QLabel* m_udpEndpointLabel;
    QLabel* m_udpSeenLabel;
    QLabel* m_rttLabel;
    QLabel* m_maxRttLabel;
    QLabel* m_rxRateLabel;
    QLabel* m_txRateLabel;
    QLabel* m_droppedLabel;

    // Per-category rate labels
    QLabel* m_audioRateLabel;
    QLabel* m_fftRateLabel;
    QLabel* m_wfRateLabel;
    QLabel* m_meterRateLabel;
    QLabel* m_daxRateLabel;

    // Per-category drop labels
    QLabel* m_audioDropLabel;
    QLabel* m_fftDropLabel;
    QLabel* m_wfDropLabel;
    QLabel* m_meterDropLabel;
    QLabel* m_daxDropLabel;
    QLabel* m_audioBufferLabel;
    QLabel* m_audioBufferPeakLabel;
    QLabel* m_audioUnderrunLabel;
    QLabel* m_audioUnderrunRateLabel;
    QLabel* m_audioPacketGapLabel;
    QLabel* m_audioPacketGapMaxLabel;
    QLabel* m_audioJitterLabel;
    QLabel* m_audioStreamLabel;
    QLabel* m_audioFeedRateLabel;
    QLabel* m_audioFeedDeficitLabel;
    QLabel* m_audioLateGapLabel;
    QLabel* m_audioStreamHealthLabel;
    QLabel* m_audioStreamsDetailLabel;
    QLabel* m_overviewStatusValue{nullptr};
    QLabel* m_overviewLatencyValue{nullptr};
    QLabel* m_overviewLossValue{nullptr};
    QLabel* m_overviewAudioValue{nullptr};

    TimeSeriesGraphWidget* m_overviewLatencyGraph{nullptr};
    TimeSeriesGraphWidget* m_overviewLossGraph{nullptr};
    TimeSeriesGraphWidget* m_overviewRatesGraph{nullptr};
    TimeSeriesGraphWidget* m_overviewAudioGraph{nullptr};
    TimeSeriesGraphWidget* m_latencyGraph{nullptr};
    TimeSeriesGraphWidget* m_ratesGraph{nullptr};
    TimeSeriesGraphWidget* m_lossGraph{nullptr};
    TimeSeriesGraphWidget* m_audioGraph{nullptr};
    TimeSeriesGraphWidget* m_audioFeedGraph{nullptr};
    QTableWidget* m_audioStreamsTable{nullptr};

    QPlainTextEdit* m_logViewer{nullptr};
    QLabel* m_logPathLabel{nullptr};
    QPushButton* m_logLiveToggle{nullptr};
    QVector<QCheckBox*> m_logCategoryCheckboxes;
    QVector<LogLine> m_logLines;
    QSet<QString> m_visibleLogCategories;
    QFile m_logFile;
    QByteArray m_logPartialLine;
    QString m_lastReopenFailurePath;
    qint64 m_logOffset{0};
    bool m_logFollowLive{true};
    bool m_handlingLogScroll{false};
};

} // namespace AetherSDR
