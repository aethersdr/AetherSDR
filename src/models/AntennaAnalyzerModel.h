#pragma once

#include "core/RigExpertHidTransport.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

namespace AetherSDR {

// A standalone instrument session, independent of RadioModel and IRadioBackend.
// Only finite FRX acquisition is supported; see docs/rigexpert-usb-poc.md.
class AntennaAnalyzerModel final : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Identifying, Ready, SettingFrequency, SettingWidth,
                       Sweeping, Closing, Complete, Cancelled, Error };
    using DeviceSource = std::function<QList<RigExpertUsbDevice>()>;
    using TransportFactory = std::function<RigExpertHidTransport*(QObject*)>;

    explicit AntennaAnalyzerModel(QObject* parent = nullptr);
    ~AntennaAnalyzerModel() override;
    void setUsbBackend(DeviceSource source, TransportFactory factory);
    bool refreshDevices();
    QList<RigExpertUsbDevice> devices() const { return m_devices; }
    bool connectDevice(int index);
    bool sweep(qint64 startHz, qint64 stopHz, int intervals, bool rfIsolated);
    void stop();
    void disconnectDevice();
    State state() const { return m_state; }
    QString stateText() const;
    QString lastError() const { return m_error; }
    QJsonObject snapshot() const;
    QJsonObject automation(const QString& action, const QString& value);

    static QByteArray commandReport(const QByteArray& command);
    static QJsonObject impedanceSample(const QByteArray& line);

signals:
    void changed();

private:
    friend class AntennaAnalyzerModelTestAccess;
    void receive(const QByteArray& report);
    void lineReceived(const QByteArray& line);
    void sendCommand(const QByteArray& command);
    void finish(State result, const QString& error = {});
    void deadlineExpired();
    void record(const QString& direction, const QByteArray& report);
    bool reject(const QString& error);

    DeviceSource m_source;
    TransportFactory m_factory;
    QList<RigExpertUsbDevice> m_devices;
    RigExpertUsbDevice m_selected;
    QPointer<RigExpertHidTransport> m_port;
    State m_state{State::Disconnected};
    State m_terminal{State::Disconnected};
    QTimer m_deadline;
    QTimer m_identityQuiet;
    QElapsedTimer m_sweepElapsed;
    QString m_error;
    QString m_identity;
    QByteArray m_expectedCommand;
    bool m_commandWritten{false};
    QByteArray m_pending;
    QJsonArray m_info;
    QJsonArray m_samples;
    QJsonArray m_trace;
    qint64 m_startHz{0};
    qint64 m_stopHz{0};
    int m_intervals{0};
    quint64 m_generation{0};
};

} // namespace AetherSDR
