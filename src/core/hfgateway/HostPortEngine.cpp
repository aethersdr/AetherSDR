#include "core/hfgateway/HostPortEngine.h"

#include <algorithm>

namespace AetherSDR::HfGateway {

using ModemHost::Client;
using ModemHost::Notification;
using ModemHost::NotificationType;
using ModemHost::Request;

HostPortEngine::HostPortEngine(const EngineConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
{
    connect(&m_client, &Client::stateChanged, this, &HostPortEngine::onState);
    connect(&m_client, &Client::replied, this, &HostPortEngine::onReplied);
    connect(&m_client, &Client::notified, this, &HostPortEngine::onNotified);
    connect(&m_client, &Client::problem, this, [this](const QString& text) { emit logLine(text, true); });
    // Stage 1 carries no session data; anything on the data port is dropped.
    connect(&m_client, &Client::dataReceived, this, [this](const QByteArray& bytes) {
        emit logLine(tr("%n byte(s) on the data port ignored (receive-only)", nullptr, static_cast<int>(bytes.size())),
                     true);
    });
}

void HostPortEngine::connectToModem()
{
    if (!isLoopbackHost(m_config.host)) {
        emit logLine(tr("%1 is not this computer: a remote modem that keys would ask this station to transmit")
                         .arg(m_config.host),
                     true);
    }
    emit logLine(tr("Connecting to %1:%2").arg(m_config.host).arg(m_config.commandPort), false);
    m_client.connectToModem(m_config.host, m_config.commandPort, m_config.dataPort);
}

void HostPortEngine::disconnectFromModem()
{
    m_client.disconnectFromModem();
}

void HostPortEngine::setMonitor(bool on)
{
    if (!monitorSupported() || on == m_monitor) {
        return;
    }
    m_monitor = on;
    if (m_client.state() == Client::State::Connected) {
        m_client.send(on ? Request::MonitorOn : Request::MonitorOff);
    }
    emit changed();
}

void HostPortEngine::clearHeard()
{
    m_heard.clear();
    emit heardChanged();
}

void HostPortEngine::onState(Client::State state)
{
    if (state == Client::State::Connected) {
        emit logLine(tr("Connected"), false);
        // Receive-only: make sure the modem isn't answering calls through us.
        m_client.send(Request::ListenOff);
        m_client.send(Request::Version);
        if (m_monitor && monitorSupported()) {
            m_client.send(Request::MonitorOn);
        }
    } else if (state == Client::State::Disconnected) {
        m_version.clear();
        m_busy = false;
        m_hasSn = false;
        emit logLine(tr("Disconnected"), false);
    }
    emit changed();
}

void HostPortEngine::onReplied(Request request, bool ok, const QString& version)
{
    if (request == Request::Version && ok) {
        m_version = version;
        emit changed();
        return;
    }
    if (!ok) {
        emit logLine(tr("Modem refused %1").arg(ModemHost::toString(request)), true);
    }
}

void HostPortEngine::onNotified(const Notification& n)
{
    switch (n.type) {
    case NotificationType::Busy:
        m_busy = n.on;
        emit changed();
        break;
    case NotificationType::SignalToNoise:
        m_hasSn = true;
        m_snDb = n.snDb;
        emit changed();
        break;
    case NotificationType::Version:
        m_version = n.text;
        emit changed();
        break;
    case NotificationType::Ptt:
        if (n.on) {
            emit logLine(tr("The modem reports it is transmitting. The gateway is receive-only and did not key the "
                            "radio; check the modem's own PTT settings."),
                         true);
        }
        break;
    case NotificationType::Pending:
        emit logLine(tr("Incoming call pending (not answered: receive-only)"), false);
        break;
    case NotificationType::CqFrame:
        noteHeard(n.source, QStringLiteral("CQ"), n.bandwidthHz, 0.0);
        break;
    case NotificationType::Monitor:
        if (!n.monitorFrom.isEmpty()
            && (n.monitorKind == QLatin1String("CALL") || n.monitorKind == QLatin1String("CQ"))) {
            noteHeard(n.monitorFrom, n.monitorKind, n.bandwidthHz, n.monitorSnDb);
        }
        break;
    case NotificationType::Connected:
        // Can't come from this connection: it sent LISTEN OFF and never
        // CONNECT. Another program drove the modem. Ending the session would
        // make the modem transmit, so a receive-only page only reports it.
        emit logLine(tr("The modem reports a session %1 to %2 that the gateway did not start")
                         .arg(n.source, n.destination),
                     true);
        break;
    default:
        break;
    }
}

void HostPortEngine::noteHeard(const QString& call, const QString& kind, int bandwidthHz, double snDb)
{
    if (call.isEmpty()) {
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto it = std::find_if(m_heard.begin(), m_heard.end(), [&call](const HeardStation& h) { return h.callsign == call; });
    if (it != m_heard.end()) {
        HeardStation h = *it;
        m_heard.erase(it);
        h.kind = kind;
        if (bandwidthHz > 0) {
            h.bandwidthHz = bandwidthHz;
        }
        h.snDb = snDb;
        h.lastHeardUtc = now;
        ++h.count;
        m_heard.prepend(h);
    } else {
        m_heard.prepend(HeardStation{call, kind, bandwidthHz, snDb, now, 1});
        while (m_heard.size() > kMaxHeard) {
            m_heard.removeLast();
        }
    }
    emit heardChanged();
}

}  // namespace AetherSDR::HfGateway
