#pragma once

#include "DxSpot.h"
#include "WsprAgent.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;
class QTimer;

namespace AetherSDR {

// Talks to the WSPR receiver on the radio (WsprAgent.h): its status with the
// spots since a time, and its settings. Plain HTTP to AetherSDR's own
// service on the radio, not SmartSDR wire. The receiver is reached at the
// radio's address, on the LAN or through a tailnet shim that publishes it.
class WsprAgentClient : public QObject {
    Q_OBJECT

public:
    explicit WsprAgentClient(QObject* parent = nullptr);

    // A new address retires every request in flight: none of them signals.
    void setRadioAddress(const QHostAddress& address);
    QHostAddress radioAddress() const { return m_address; }

    // Status, with the spots and cycles from `since` (UTC seconds) on.
    void fetchStatus(qint64 since = 0);
    void fetchConfig();
    // The first save claims the receiver and yields its admin token
    // (configSaved's `newToken`); later ones need it.
    void saveConfig(const WsprConfig& config, const QString& adminToken);
    // Retires every request in flight.
    void cancel();

signals:
    void statusReceived(const AetherSDR::WsprStatus& status);
    void configReceived(const AetherSDR::WsprConfig& config, bool claimed);
    void configSaved(const AetherSDR::WsprConfig& config, const QString& newToken);
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

// Reads the receiver's spots for SpotHub and the panadapter: polls its
// status every 30 s and emits each spot it hasn't seen yet, as a DxSpot in
// the WSPR source's color and lifetime. Starts with the spots still within
// that lifetime, so a restart doesn't flood the panadapter with old ones.
class WsprSpotFeed : public QObject {
    Q_OBJECT

public:
    explicit WsprSpotFeed(QObject* parent = nullptr);

    void start(const QHostAddress& radio);
    void stop();
    bool isRunning() const { return m_running; }
    QHostAddress radioAddress() const { return m_client->radioAddress(); }

signals:
    void started();
    void stopped();
    void statusReceived(const AetherSDR::WsprStatus& status);
    // Every new decode, hashed calls included, with the receiving station's
    // grid for its distance: for SpotHub's console.
    void decoded(const AetherSDR::WsprSpot& spot, const QString& receiverGrid);
    // A new decode with a resolved callsign, for the panadapter.
    void spotReceived(const AetherSDR::DxSpot& spot);
    void failed(const QString& message);

private:
    void poll();
    void onStatus(const WsprStatus& status);

    WsprAgentClient* m_client{nullptr};
    QTimer* m_timer{nullptr};
    bool m_running{false};
    qint64 m_since{0};           // the first cycle start not yet emitted
};

namespace wspr {
WsprSpotHubSettings loadSpotHubSettings();
void saveSpotHubSettings(const WsprSpotHubSettings& settings);
}  // namespace wspr

}  // namespace AetherSDR
