#pragma once

#include "core/hfgateway/ModemHostProtocol.h"
#include "core/hfgateway/ModemHostSession.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QTcpSocket;

// One external modem, reached over its VARA-style command and data ports.
// A thin socket shell around ModemHost::Session, which holds the logic that
// matters (reply matching, transmit permission) and is tested without
// sockets.
//
// Connecting is always an explicit operator action, never automatic. Some
// modems accept one client per port (Mercury docs/TNC.md, "One client at a
// time"), so a gateway connection locks any other host program out of that
// modem for as long as it is held.
//
// Untrusted input (Principle VII): command-port lines go through
// ModemHost::LineSplitter's caps, and a modem that sends too many rejected
// lines is disconnected. Outbound data is refused once the data socket's
// unsent backlog would pass kMaxDataBacklogBytes.

namespace AetherSDR::ModemHost {

class Client : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Connected };

    // Unsent bytes allowed to pile up on the data socket.
    static constexpr qint64 kMaxDataBacklogBytes = 256 * 1024;
    // Rejected command-port lines tolerated per connection before it is closed.
    static constexpr int kMaxRejectedLines = 8;

    explicit Client(QObject* parent = nullptr);
    ~Client() override;

    // Opens both sockets. Hosts other than loopback are allowed but the caller
    // is expected to have warned: a remote modem that reports PTT ON would
    // ask this station to key.
    void connectToModem(const QString& host, quint16 commandPort, quint16 dataPort);
    void disconnectFromModem();
    State state() const { return m_state; }

    void setTransmitPermitted(bool permitted) { m_session.setTransmitPermitted(permitted); }
    bool transmitPermitted() const { return m_session.transmitPermitted(); }

    // False, and nothing sent, when the session refuses (see Session::refusal())
    // or the client is not connected.
    bool send(Request request, const QStringList& callsigns = {});
    Refusal lastRefusal() const { return m_session.refusal(); }

    // False, and nothing written, when not permitted, not connected or the
    // backlog cap would be passed.
    bool writeData(const QByteArray& bytes);

signals:
    void stateChanged(AetherSDR::ModemHost::Client::State state);
    void replied(AetherSDR::ModemHost::Request request, bool ok, const QString& version);
    void notified(const AetherSDR::ModemHost::Notification& notification);
    void dataReceived(const QByteArray& bytes);
    // Something went wrong in the protocol or the connection; the text is for
    // the operator's log.
    void problem(const QString& text);

private:
    void setState(State s);
    void onCommandReadyRead();
    void onSocketConnected();
    void onSocketLost(const QString& why);
    void closeSockets();

    QTcpSocket* m_cmd = nullptr;
    QTcpSocket* m_data = nullptr;
    LineSplitter m_splitter;
    Session m_session;
    State m_state = State::Disconnected;
};

}  // namespace AetherSDR::ModemHost
