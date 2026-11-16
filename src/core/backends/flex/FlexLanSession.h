#pragma once

#include "core/backends/IRadioBackend.h"
#include "FlexCommandSession.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

namespace AetherSDR {

class RadioConnection;
class PanadapterStream;
class FlexStatusIngress;

// Backend-private coordinator for the existing TCP and UDP workers. Policy
// (retry, GUI registration, settings) stays with the session's consumer.
class FlexLanSession final : public QObject {
    Q_OBJECT
    friend class FlexLanSessionTestAccess;
public:
    FlexLanSession(RadioConnection* connection, PanadapterStream* stream,
                   FlexStatusIngress* ingress, QObject* parent = nullptr);
    ~FlexLanSession() override;
    void connectRadio(const RadioConnectRequest& request);
    void disconnectRadio(const RadioDisconnectRequest& request = {}, quint32 sequence = 0);
    bool isConnected() const { return m_connected; }
    bool active() const { return m_active; }
    std::optional<ConnectionSession> snapshot() const { return m_snapshot; }
    ReceiveTransportResult startReceiveTransport(bool rebind);
    FlexCommandSession& commands() { return m_commands; }

signals:
    void starting(quint64 epoch);
    void connected();
    void disconnected();
    void errorOccurred(const QString& message);
    void versionReceived(const QString& version);

private:
    bool current(quint64 epoch) const;
    void finish(quint64 epoch);
    void stopReceiveTransport();
    RadioConnection* m_connection;
    PanadapterStream* m_stream;
    FlexStatusIngress* m_ingress;
    std::shared_ptr<std::atomic<quint64>> m_epoch;
    bool m_active{false};
    bool m_connected{false};
    bool m_synthetic{false};
    std::optional<ConnectionSession> m_snapshot;
    RadioConnectRequest m_request;
    FlexCommandSession m_commands;
};

} // namespace AetherSDR
