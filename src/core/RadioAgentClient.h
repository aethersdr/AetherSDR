#pragma once

#include "DxSpot.h"

#include <QByteArray>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;
class QTimer;

namespace AetherSDR {

// A receiver AetherSDR runs on the radio and reads over HTTP: AetherWSPR
// (WsprAgent.h) and AetherFT8 (Ft8Agent.h). Both answer GET /v1/status,
// GET /v1/config and POST /v1/config on a port of their own, at the radio's
// address, on the LAN or through a tailnet shim that publishes it.
//
// What is the same for every such receiver lives here, once: the request,
// its safety properties and the polling. What differs, the JSON each one
// speaks, lives in its own subclass (WsprAgentClient, Ft8AgentClient).
struct RadioAgent {
    const char* name;    // "WSPR", "FT8": the receiver as messages name it
    quint16 port;
    int pollMs;          // how often its spot feed asks for new decodes
};

// Plain HTTP to one receiver on the radio, not SmartSDR wire.
//
// Safety properties, the same for every receiver: a transfer timeout,
// redirects never followed (one would carry the admin token in its
// Authorization header to wherever it pointed), a reply size cap, one status
// poll at a time, and a generation counter so a request retired by a new
// address or cancel() never signals.
class RadioAgentClient : public QObject {
    Q_OBJECT

public:
    RadioAgentClient(const RadioAgent& agent, QObject* parent = nullptr);

    const RadioAgent& agent() const { return m_agent; }

    // A new address retires every request in flight: none of them signals.
    void setRadioAddress(const QHostAddress& address);
    QHostAddress radioAddress() const { return m_address; }

    // Status, with the spots and cycles from `since` (UTC seconds) on. At
    // most one is in flight; another asked for meanwhile is dropped.
    void fetchStatus(qint64 since = 0);
    void fetchConfig();
    // Retires every request in flight.
    void cancel();

signals:
    // `operation` is "status", "config" or "save". `unauthorized`: the
    // receiver refused the admin token.
    void requestFailed(const QString& operation, const QString& message, bool unauthorized);
    // From every status read: the radio's index for the receiver's own
    // slice, -1 while it has none. The radio reports that slice as the GUI
    // client's, so this is how AetherSDR knows to leave it alone (#7139).
    void receiverSlice(int sliceIndex);

protected:
    // POST /v1/config. The first one claims the receiver and its reply
    // carries the admin token; later ones need that token.
    void postConfig(const QByteArray& json, const QString& adminToken);
    // A 200 reply, for the subclass to parse. Not called for a retired request.
    virtual void received(const QString& operation, const QByteArray& body) = 0;
    void fail(const QString& operation, const QString& message, bool unauthorized = false);
    // "The WSPR receiver", "The FT8 receiver": for messages.
    QString receiverName() const;

private:
    QNetworkReply* send(const QByteArray& method, const QString& pathAndQuery, const QByteArray& body,
                        const QString& adminToken);
    void handle(QNetworkReply* reply, const QString& operation);

    RadioAgent m_agent;
    QNetworkAccessManager m_nam;
    QHostAddress m_address;
    quint64 m_generation{0};
    QPointer<QNetworkReply> m_statusReply;   // one status poll at a time
};

// The receiver answers LAN callers, and a tailnet shim publishes it: a
// private, link-local or Tailscale (100.64.0.0/10) IPv4 address.
bool radioAgentReachable(const QHostAddress& address);

class RadioModel;
class RadioAgentSpotFeed;
// Every status the feed reads reserves the slice the receiver names (#7139),
// under `label`, so a missed read after a new slice heals on the next poll.
QMetaObject::Connection reserveReceiverSlice(RadioAgentSpotFeed* feed, RadioModel* radio,
                                             const QString& label);

// Polls one receiver for its decodes and turns them into SpotHub and
// panadapter spots. The polling is shared; the subclass reads the status
// (WsprSpotFeed, Ft8SpotFeed).
class RadioAgentSpotFeed : public QObject {
    Q_OBJECT

public:
    // Takes ownership of `client`.
    RadioAgentSpotFeed(RadioAgentClient* client, QObject* parent = nullptr);

    void start(const QHostAddress& radio);
    void stop();
    bool isRunning() const { return m_running; }
    QHostAddress radioAddress() const { return m_client->radioAddress(); }

signals:
    void started();
    void stopped();
    // A new decode with a callsign, for the panadapter.
    void spotReceived(const AetherSDR::DxSpot& spot);
    // The subclass's recent-decode list gained this poll's decodes.
    void spotsUpdated();
    void failed(const QString& message);
    // RadioAgentClient::receiverSlice, from every status the feed reads.
    void receiverSlice(int sliceIndex);

protected:
    // The first cycle start the feed asks for when it starts.
    virtual qint64 firstCycle() const = 0;
    // A different radio: forget the last one's decodes.
    virtual void forgetRecent() = 0;

    RadioAgentClient* client() const { return m_client; }

    qint64 m_since{0};           // the first cycle start not yet emitted

private:
    void poll();

    RadioAgentClient* m_client{nullptr};
    QTimer* m_timer{nullptr};
    bool m_running{false};
};

}  // namespace AetherSDR
