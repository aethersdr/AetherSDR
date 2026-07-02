#pragma once

#include <QHostAddress>
#include <QObject>
#include <QProcess>
#include <QString>

namespace AetherSDR {

class DStarWaveformProcess : public QObject
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

    static DStarWaveformProcess& instance();

    State state() const { return m_state; }
    bool isActive() const { return m_state == State::Starting || m_state == State::Running; }
    QString statusText() const { return m_statusText; }
    QString lastError() const { return m_lastError; }

    static QString stateName(State state);
    static QString defaultExecutablePath();
    static QString resolveExecutablePath(const QString& configuredPath);

public slots:
    bool startForRadio(const QHostAddress& radioAddress);
    void stop();

signals:
    void stateChanged(AetherSDR::DStarWaveformProcess::State state);
    void statusTextChanged(const QString& text);
    void processOutput(const QString& line);

private:
    explicit DStarWaveformProcess(QObject* parent = nullptr);

    void setState(State state, const QString& statusText = {});
    void fail(const QString& message);
    QStringList launchArguments(const QHostAddress& radioAddress) const;
    void drainOutput(QProcess::ProcessChannel channel);

    QProcess m_process;
    State m_state{State::Stopped};
    QString m_statusText;
    QString m_lastError;
    QString m_runningBackendLabel;
};

} // namespace AetherSDR
