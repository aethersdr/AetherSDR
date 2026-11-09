#pragma once

#include "Ft8Agent.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;

namespace AetherSDR {

// Talks to the FT8 receiver on the radio (Ft8Agent.h): its status with the
// spots since a time, and its settings. Plain HTTP to AetherSDR's own service
// on the radio, not SmartSDR wire. The receiver is reached at the radio's
// address, on the LAN or through a tailnet shim that publishes it.
//
// Mirrors WsprAgentClient deliberately, including its safety properties: a
// transfer timeout, redirects never followed (one would carry the admin token
// in its Authorization header to wherever it pointed), a reply size cap, and
// a generation counter so a retired request cannot signal.
class Ft8AgentClient : public QObject {
    Q_OBJECT

public:
    explicit Ft8AgentClient(QObject* parent = nullptr);

    // A new address retires every request in flight: none of them signals.
    void setRadioAddress(const QHostAddress& address);
    QHostAddress radioAddress() const { return m_address; }

    // Status, with the spots and cycles from `since` (UTC seconds) on.
    void fetchStatus(qint64 since = 0);
    void fetchConfig();
    // The first save claims the receiver and yields its admin token
    // (configSaved's `newToken`); later ones need it.
    void saveConfig(const Ft8Config& config, const QString& adminToken);
    // Retires every request in flight.
    void cancel();

signals:
    void statusReceived(const AetherSDR::Ft8Status& status);
    void configReceived(const AetherSDR::Ft8Config& config, bool claimed);
    void configSaved(const AetherSDR::Ft8Config& config, const QString& newToken);
    // `operation` is "status", "config" or "save". `unauthorized`: the
    // receiver refused the admin token.
    void requestFailed(const QString& operation, const QString& message, bool unauthorized);

private:
    QNetworkReply* send(const QByteArray& method, const QString& pathAndQuery, const QByteArray& body,
                        const QString& adminToken);
    void handle(QNetworkReply* reply, const QString& operation);

    QNetworkAccessManager m_nam;
    QHostAddress m_address;
    quint64 m_generation{0};
    QPointer<QNetworkReply> m_statusReply;   // one status poll at a time
};

}  // namespace AetherSDR
