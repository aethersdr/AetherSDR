#pragma once

#include "core/backends/IRadioBackend.h"
#include "WanConnection.h"
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>

namespace AetherSDR {
class PanadapterStream;
class FlexStatusIngress;

// Owns one WAN attempt from UDP allocation through broker/TLS handshake and
// bounded teardown. Account authentication outlives this transport lease.
class FlexWanSession final : public QObject {
    Q_OBJECT
    friend class FlexWanSessionTestAccess;
public:
    FlexWanSession(PanadapterStream* stream, FlexStatusIngress* ingress, QObject* parent = nullptr);
    ~FlexWanSession() override;
    void connectRadio(const RemoteRadioRequest& request);
    void disconnectRadio(const RadioDisconnectRequest& request = {});
    bool active() const { return m_epoch != 0; }
    bool closing() const { return m_closing; }
    bool isConnected() const { return m_snapshot.has_value() && !m_closing; }
    std::optional<ConnectionSession> snapshot() const { return m_snapshot; }
    ReceiveTransportResult startReceiveTransport();
    FlexCommandSession& commands() { return m_connection.commands(); }
    void decideCertificate(quint64 attempt, bool accept);

signals:
    void starting(quint64 epoch);
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void versionReceived(const QString& version);
    void pingMeasured(int milliseconds);
    void certificateRequired(quint64 attempt, const QString& host,
                             const QString& expected, const QString& presented);
private:
    bool current(quint64 epoch) const { return epoch && m_epoch == epoch && !m_closing; }
    void finish(quint64 epoch);
    void fail(quint64 epoch, const QString& message);
    void closeSocket(quint64 epoch);
    void stopReceive();
    std::function<ReceiveTransportResult(const WanRadioInfo&)> m_prepareReceive;
    std::function<void()> m_stopReceive;
    std::function<void(const QString&, quint16, const QString&)> m_dial;
    PanadapterStream* m_stream;
    FlexStatusIngress* m_ingress;
    RemoteRadioRequest m_request;
    quint64 m_epoch{0};
    quint64 m_lease{0};
    bool m_closing{false};
    quint16 m_udpPort{0};
    quint64 m_certificateAttempt{0};
    std::optional<ConnectionSession> m_snapshot;
    QTimer m_deadline;
    WanConnection m_connection;
};
} // namespace AetherSDR
