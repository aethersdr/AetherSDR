#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace AetherSDR {

enum class DigitalVoiceWaveformHealth {
    Inactive,
    Measuring,
    Healthy,
    CadenceDegraded,
    TransportLoss,
    SourceDeficits
};

enum class DigitalVoiceWaveformMetricDirection {
    Rx,
    Tx
};

struct DigitalVoiceWaveformMetrics {
    bool valid{false};
    DigitalVoiceWaveformMetricDirection direction{
        DigitalVoiceWaveformMetricDirection::Rx};
    quint64 generation{0};
    quint64 reportSequence{0};
    qint64 timestampMs{0};
    QString mode;
    double rxSampleRateHz{0.0};
    quint32 vitaSequenceGaps{0};
    quint32 sourceBlockDeficits{0};
    double turnaroundMeanUs{0.0};
    quint64 turnaroundMaxUs{0};
    quint32 queueMax{0};
    quint64 vitaSequenceGapsTotal{0};
    quint64 sourceBlockDeficitsTotal{0};
    bool txValid{false};
    qint64 txTimestampMs{0};
    double txSampleRateHz{0.0};
    quint32 txVitaSequenceGaps{0};
    quint32 txNullFrames{0};
    quint32 txPcmClips{0};
    quint32 txPcmInvalid{0};
    quint32 txSendFailures{0};
    quint32 txQueueMax{0};
    quint32 txTailSamples{0};
    quint64 txTailUs{0};
    quint32 txPreRollFrames{0};
    quint32 txPreRollDelayMs{0};
    quint32 txAmbeQueueMax{0};
    quint32 txAmbeUnderflows{0};
    quint32 txAmbeOverflows{0};
    quint32 txAmbeSequenceErrors{0};
    quint32 txVocoderSubmitFailures{0};
    quint32 txVocoderPendingMax{0};
    quint32 txDrainFrames{0};
    quint32 txDrainTimeouts{0};
    quint32 txDrainDiscardedFrames{0};
    quint64 txVitaSequenceGapsTotal{0};
    quint64 txNullFramesTotal{0};
    quint64 txPcmClipsTotal{0};
    quint64 txPcmInvalidTotal{0};
    quint64 txSendFailuresTotal{0};
    quint64 txAmbeUnderflowsTotal{0};
    quint64 txAmbeOverflowsTotal{0};
    quint64 txAmbeSequenceErrorsTotal{0};
    quint64 txVocoderSubmitFailuresTotal{0};
    quint64 txDrainTimeoutsTotal{0};
    quint64 txDrainDiscardedFramesTotal{0};
};

bool isDegradedDigitalVoiceWaveformHealth(DigitalVoiceWaveformHealth health);

class DigitalVoiceWaveformHealthTracker
{
public:
    static constexpr double kExpectedSampleRateHz = 24000.0;
    static constexpr double kCadenceWarningRateHz =
        kExpectedSampleRateHz * 0.99;
    static constexpr int kLowCadenceWindows = 5;
    static constexpr int kSourceDeficitWindows = 2;
    static constexpr int kRecoveryWindows = 10;

    void reset();
    DigitalVoiceWaveformHealth observe(const DigitalVoiceWaveformMetrics& metrics);
    DigitalVoiceWaveformHealth health() const { return m_health; }

private:
    DigitalVoiceWaveformHealth m_health{DigitalVoiceWaveformHealth::Inactive};
    int m_observationCount{0};
    int m_lowCadenceStreak{0};
    int m_sourceDeficitStreak{0};
    int m_healthyStreak{0};
};

class DigitalVoiceWaveformTelemetryParser
{
public:
    static constexpr qsizetype kMaximumMetricLineBytes = 1024;
    static constexpr qsizetype kMaximumBufferedLineBytes = 4096;

    QList<QByteArray> append(const QByteArray& chunk);
    QList<QByteArray> finish();
    void reset();

    static bool isMetricLine(const QByteArray& line);
    static bool parseMetricLine(const QByteArray& line,
                                DigitalVoiceWaveformMetrics* metrics);

private:
    QList<QByteArray> takeCompleteLines();

    QByteArray m_buffer;
    bool m_discardUntilNewline{false};
};

} // namespace AetherSDR
