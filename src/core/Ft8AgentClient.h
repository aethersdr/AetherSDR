#pragma once

#include "Ft8Agent.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

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

// Polls the FT8 receiver and turns its decodes into spots.
//
// The same shape as WsprSpotFeed, and deliberately a narrow copy of it rather
// than a shared base: see the note at the top of Ft8Agent.h for why the WSPR
// pair was copied instead of abstracted. What differs is the cycle (15 s, not
// 120 s) and that an FT8 decode carries no dB SNR.
class Ft8SpotFeed : public QObject {
    Q_OBJECT

public:
    explicit Ft8SpotFeed(QObject* parent = nullptr);

    void start(const QHostAddress& radio);
    void stop();
    bool isRunning() const { return m_running; }
    QHostAddress radioAddress() const { return m_client->radioAddress(); }

    const QList<Ft8Spot>& recentSpots() const { return m_recent; }
    QString receiverCallsign() const { return m_receiverCall; }

signals:
    void started();
    void stopped();
    void statusReceived(const AetherSDR::Ft8Status& status);
    // Every new decode, for a console that wants them all.
    void decoded(const AetherSDR::Ft8Spot& spot);
    // A new decode with a callsign, for the panadapter.
    void spotReceived(const AetherSDR::DxSpot& spot);
    // recentSpots() gained this poll's decodes.
    void spotsUpdated();
    void failed(const QString& message);

private:
    void poll();
    void onStatus(const Ft8Status& status);

    Ft8AgentClient* m_client{nullptr};
    QTimer* m_timer{nullptr};
    bool m_running{false};
    qint64 m_since{0};           // the first cycle start not yet emitted
    QList<Ft8Spot> m_recent;
    QString m_receiverCall;
};

}  // namespace AetherSDR
