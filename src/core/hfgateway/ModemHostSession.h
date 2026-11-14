#pragma once

#include "core/hfgateway/ModemHostProtocol.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <deque>

// The client side of one connection to an external modem's command port, with
// no sockets: it builds the commands the gateway may send, matches the modem's
// positional replies to them, and enforces the transmit permission.
//
// Replies are positional (docs/hf-gateway-host-interface.md, Transport), but
// not every command is answered with OK/WRONG: VERSION is answered with a
// VERSION line or WRONG. The queue records what each outstanding command
// expects. BUFFER, SN and BITRATE are never sent as queries, because the same
// lines also arrive unsolicited and Data2G answers some of them WRONG; the
// gateway relies on the unsolicited reports instead.
//
// Transmit permission. LISTEN ON lets a modem answer a call, which keys the
// radio, so it is refused unless the owner has granted permission. Ending
// things (LISTEN OFF, DISCONNECT, ABORT) and setting MYCALL are always allowed.
// The session never offers CONNECT, CQFRAME or TUNE at all. Permission starts
// off and revoking it does not send anything by itself; the owner decides
// what to send (LISTEN OFF).

namespace AetherSDR::ModemHost {

enum class Request {
    MyCall,
    ListenOn,
    ListenOff,
    Disconnect,
    Abort,
    Version,
};

enum class Refusal {
    None,
    NotPermitted,  // needs transmit permission
    QueueFull,     // kMaxOutstanding replies already awaited
    Invalid,       // bad arguments (e.g. MYCALL callsigns)
};

class Session {
public:
    static constexpr int kMaxOutstanding = 16;

    struct Event {
        enum class Kind {
            Reply,         // `request` answered; `ok` says OK or WRONG
            Notification,  // unsolicited line, in `notification`
            Unmatched,     // OK/WRONG with nothing outstanding: a protocol error
        };
        Kind kind = Kind::Notification;
        Request request = Request::Version;
        bool ok = false;
        QString version;            // Reply to Version, when ok
        Notification notification;  // Notification
    };

    void setTransmitPermitted(bool permitted) { m_permitted = permitted; }
    bool transmitPermitted() const { return m_permitted; }

    // Bytes to write for `request`, or empty with refusal() set. `callsigns`
    // is used by MyCall only.
    QByteArray request(Request request, const QStringList& callsigns = {});
    Refusal refusal() const { return m_refusal; }

    // Data-port writes carry session payload, which a modem only sends inside
    // a session it was permitted to answer.
    bool mayWriteData() const { return m_permitted; }

    // Feed one framed command-port line from the modem.
    Event onLine(const QString& line);

    int outstanding() const { return static_cast<int>(m_awaiting.size()); }

    // The connection closed: drop every expectation.
    void reset();

private:
    std::deque<Request> m_awaiting;
    bool m_permitted = false;
    Refusal m_refusal = Refusal::None;
};

QString toString(Request request);

}  // namespace AetherSDR::ModemHost
