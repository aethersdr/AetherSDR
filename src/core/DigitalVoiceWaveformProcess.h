#pragma once

#include "DigitalVoiceModeRegistry.h"
#include "DigitalVoiceWaveformTelemetry.h"

#include <QHostAddress>
#include <QObject>
#include <QProcess>
#include <QString>

namespace AetherSDR {

class DigitalVoiceWaveformProcess : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Starting,
        Running,
        Stopping,
        Failed
    };
    Q_ENUM(State)

    static DigitalVoiceWaveformProcess& instance();

    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Starting || m_state == State::Running; }
    QString statusText() const { return m_statusText; }
    QString lastError() const { return m_lastError; }
    QString activeModeName() const;
    QString registrationName() const;
    bool registrationVerified() const { return m_registrationVerified; }
    const DigitalVoiceWaveformMetrics& metrics() const { return m_metrics; }
    DigitalVoiceWaveformHealth health() const { return m_health; }
    QString healthDetail() const;

    static QString stateName(State state);
    static QString healthName(DigitalVoiceWaveformHealth health);
    static QString defaultExecutablePath();
    static QString resolveExecutablePath(const QString& configuredPath);

public slots:
    bool startForRadio(const QHostAddress& radioAddress,
                       const QString& radioCallsign = {},
                       DigitalVoiceModeId mode = DigitalVoiceModeId::DStar);
    void stop();
    void stopAndWait();

signals:
    void stateChanged(AetherSDR::DigitalVoiceWaveformProcess::State state);
    void statusTextChanged(const QString& text);
    void metricsChanged();
    void healthChanged();
    void degradationStarted(const QString& message);
    void processOutput(const QString& line);
    void sliceRestoreRequested(int sliceId, const QString& previousMode);

private:
    explicit DigitalVoiceWaveformProcess(QObject* parent = nullptr);

    void setState(State state, const QString& statusText = {});
    void fail(const QString& message);
    void deactivateModeAndRequestRestore();
    QStringList launchArguments(const QHostAddress& radioAddress,
                                const QString& effectiveMyCall,
                                const DigitalVoiceModeDescriptor& mode) const;
    void drainOutput(QProcess::ProcessChannel channel, bool flush = false);
    void processLines(const QList<QByteArray>& lines);
    void updateMetrics(DigitalVoiceWaveformMetrics metrics);
    void resetTelemetry(bool advanceGeneration);
    void setHealth(DigitalVoiceWaveformHealth health);

    QProcess m_process;
    State m_state{State::Stopped};
    QString m_statusText;
    QString m_lastError;
    QString m_helperError;
    DigitalVoiceModeId m_mode{DigitalVoiceModeId::DStar};
    bool m_registrationVerified{false};
    quint64 m_startGeneration{0};
    quint64 m_stopGeneration{0};
    quint64 m_metricsGeneration{0};
    quint64 m_metricsSequence{0};
    quint64 m_vitaSequenceGapsTotal{0};
    quint64 m_sourceBlockDeficitsTotal{0};
    quint64 m_txVitaSequenceGapsTotal{0};
    quint64 m_txNullFramesTotal{0};
    quint64 m_txPcmClipsTotal{0};
    quint64 m_txPcmInvalidTotal{0};
    quint64 m_txSendFailuresTotal{0};
    quint64 m_txAmbeUnderflowsTotal{0};
    quint64 m_txAmbeOverflowsTotal{0};
    quint64 m_txAmbeSequenceErrorsTotal{0};
    quint64 m_txVocoderSubmitFailuresTotal{0};
    quint64 m_txDrainTimeoutsTotal{0};
    quint64 m_txDrainDiscardedFramesTotal{0};
    DigitalVoiceWaveformMetrics m_metrics;
    DigitalVoiceWaveformHealth m_health{DigitalVoiceWaveformHealth::Inactive};
    DigitalVoiceWaveformHealthTracker m_healthTracker;
    DigitalVoiceWaveformTelemetryParser m_stdoutParser;
    DigitalVoiceWaveformTelemetryParser m_stderrParser;
};

} // namespace AetherSDR
