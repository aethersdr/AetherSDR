#pragma once

#include "core/TcpByteProxy.h"

#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace AetherSDR {

// Operator-facing state for the CTR2 TCP proxy prototype. Owns the
// TcpByteProxy, validates the explicit listen/radio endpoints, and freezes
// them while the proxy runs. Independent of RadioModel and every backend:
// the CTR2 is its own radio client and the proxy never touches AetherSDR's
// command connection. Nothing persists; the proxy is off on every launch.
// Design: docs/ctr2-tcp-proxy-design.md.
class Ctr2ProxyModel : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kDefaultPort = 4992;

    explicit Ctr2ProxyModel(QObject* parent = nullptr);
    ~Ctr2ProxyModel() override;

    // Local non-loopback IPv4 addresses the operator may bind to.
    QStringList availableListenAddresses() const { return m_listenChoices; }
    void refreshListenAddresses();

    // Setters refuse (return false) while running.
    QString listenAddress() const { return m_listenAddress; }
    bool setListenAddress(const QString& address);
    QString listenPortText() const { return m_listenPort; }
    bool setListenPortText(const QString& port);
    QString radioAddress() const { return m_radioAddress; }
    bool setRadioAddress(const QString& address);
    QString radioPortText() const { return m_radioPort; }
    bool setRadioPortText(const QString& port);

    // Empty when Start may be pressed; otherwise the reason it may not.
    QString configurationProblem() const;
    bool isRunning() const;

    bool start();
    void stop();

    TcpByteProxy::State state() const { return m_proxy->state(); }
    QString stateText() const { return TcpByteProxy::stateName(state()); }
    QString lastError() const { return m_proxy->lastError(); }
    QString listenerEndpoint() const { return m_proxy->listenerDescription(); }
    QString peerEndpoint() const;
    QString radioEndpoint() const { return m_proxy->upstreamDescription(); }
    TcpByteProxy::Stats stats() const { return m_proxy->stats(); }

signals:
    void configurationChanged();
    void listenAddressesChanged();
    void stateChanged();
    void endpointsChanged();
    void statsChanged();      // coalesced for display
    void lastErrorChanged();

private:
    static bool parsePort(const QString& text, quint16* port);
    static bool parseIpv4(const QString& text, QHostAddress* address);
    bool buildConfig(TcpByteProxy::Config* config, QString* problem) const;

    TcpByteProxy* m_proxy{nullptr};
    QTimer* m_statsTimer{nullptr};
    QStringList m_listenChoices;
    QString m_listenAddress;
    QString m_listenPort{QString::number(kDefaultPort)};
    QString m_radioAddress;
    QString m_radioPort{QString::number(kDefaultPort)};
};

} // namespace AetherSDR
