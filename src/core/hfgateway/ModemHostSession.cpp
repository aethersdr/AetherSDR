#include "core/hfgateway/ModemHostSession.h"

namespace AetherSDR::ModemHost {

QByteArray Session::request(Request request, const QStringList& callsigns)
{
    m_refusal = Refusal::None;
    if (request == Request::ListenOn && !m_permitted) {
        m_refusal = Refusal::NotPermitted;
        return {};
    }
    if (outstanding() >= kMaxOutstanding) {
        m_refusal = Refusal::QueueFull;
        return {};
    }

    QByteArray bytes;
    switch (request) {
    case Request::MyCall: bytes = cmdMyCall(callsigns); break;
    case Request::ListenOn: bytes = cmdListen(true); break;
    case Request::ListenOff: bytes = cmdListen(false); break;
    case Request::Disconnect: bytes = cmdDisconnect(); break;
    case Request::Abort: bytes = cmdAbort(); break;
    case Request::Version: bytes = cmdVersion(); break;
    }
    if (bytes.isEmpty()) {
        m_refusal = Refusal::Invalid;
        return {};
    }
    m_awaiting.push_back(request);
    return bytes;
}

Session::Event Session::onLine(const QString& line)
{
    Event ev;
    const Notification n = parseNotification(line);

    const bool reply = n.type == NotificationType::Ok || n.type == NotificationType::Wrong;
    if (reply) {
        if (m_awaiting.empty()) {
            ev.kind = Event::Kind::Unmatched;
            ev.notification = n;
            return ev;
        }
        ev.kind = Event::Kind::Reply;
        ev.request = m_awaiting.front();
        ev.ok = n.type == NotificationType::Ok;
        m_awaiting.pop_front();
        return ev;
    }

    // A VERSION line answers the oldest outstanding command only when that
    // command is VERSION; otherwise it is unsolicited.
    if (n.type == NotificationType::Version && !m_awaiting.empty() && m_awaiting.front() == Request::Version) {
        ev.kind = Event::Kind::Reply;
        ev.request = Request::Version;
        ev.ok = true;
        ev.version = n.text;
        m_awaiting.pop_front();
        return ev;
    }

    ev.kind = Event::Kind::Notification;
    ev.notification = n;
    return ev;
}

void Session::reset()
{
    m_awaiting.clear();
    m_refusal = Refusal::None;
}

QString toString(Request request)
{
    switch (request) {
    case Request::MyCall: return QStringLiteral("MYCALL");
    case Request::ListenOn: return QStringLiteral("LISTEN ON");
    case Request::ListenOff: return QStringLiteral("LISTEN OFF");
    case Request::Disconnect: return QStringLiteral("DISCONNECT");
    case Request::Abort: return QStringLiteral("ABORT");
    case Request::Version: return QStringLiteral("VERSION");
    }
    return QStringLiteral("?");
}

}  // namespace AetherSDR::ModemHost
