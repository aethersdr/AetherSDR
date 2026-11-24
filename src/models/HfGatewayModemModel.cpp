#include "models/HfGatewayModemModel.h"

#include "core/hfgateway/HfGatewaySettings.h"
#include "core/hfgateway/ModemHostClient.h"

#include <algorithm>

namespace AetherSDR::HfGateway {

using ModemHost::Client;
using ModemHost::Notification;
using ModemHost::NotificationType;
using ModemHost::Request;

namespace {
constexpr int kListenOffTimeoutMs = 5000;
}  // namespace

HfGatewayModemModel::HfGatewayModemModel(const EngineConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_client(std::make_unique<Client>())
{
    m_listenOffTimeout.setSingleShot(true);
    connect(&m_listenOffTimeout, &QTimer::timeout, this, [this] {
        disconnectFromModem();
        emit logLine(tr("The modem did not confirm LISTEN OFF. Connection closed."), true);
    });
    connect(m_client.get(), &Client::stateChanged, this, &HfGatewayModemModel::onStateChanged);
    connect(m_client.get(), &Client::replied, this, &HfGatewayModemModel::onReplied);
    connect(m_client.get(), &Client::notified, this, &HfGatewayModemModel::onNotified);
    connect(m_client.get(), &Client::problem, this, [this](const QString& text) { emit logLine(text, true); });
    // Stage 1 carries no session data; anything on the data port is dropped.
    connect(m_client.get(), &Client::dataReceived, this, [this](const QByteArray& bytes) {
        emit logLine(tr("%n byte(s) on the data port ignored (receive-only)", nullptr, static_cast<int>(bytes.size())),
                     true);
    });
}

HfGatewayModemModel::~HfGatewayModemModel()
{
    m_client->disconnect(this);
    monitorOffBeforeDisconnect();
    m_client->disconnectFromModem();
}

Config HfGatewayModemModel::loadConfig()
{
    return Settings::load();
}

bool HfGatewayModemModel::saveConfig(const Config& config)
{
    return Settings::save(config);
}

HfGatewayModemModel::State HfGatewayModemModel::state() const
{
    if (m_client->state() == Client::State::Disconnected) {
        return State::Disconnected;
    }
    return m_ready ? State::Connected : State::Connecting;
}

bool HfGatewayModemModel::transmitPermitted() const
{
    return m_client->transmitPermitted();
}

void HfGatewayModemModel::connectToModem()
{
    emit logLine(tr("Connecting to %1:%2").arg(m_config.host).arg(m_config.commandPort), false);
    m_client->connectToModem(m_config.host, m_config.commandPort, m_config.dataPort);
}

void HfGatewayModemModel::monitorOffBeforeDisconnect()
{
    if (m_monitorMayBeActive && m_client->state() == Client::State::Connected) {
        m_client->send(Request::MonitorOff);
    }
}

void HfGatewayModemModel::disconnectFromModem()
{
    monitorOffBeforeDisconnect();
    m_client->disconnectFromModem();
}

void HfGatewayModemModel::setMonitor(bool on)
{
    if (!monitorSupported() || on == m_monitor) {
        return;
    }
    m_monitor = on;
    if (m_ready) {
        sendMonitorSetting();
    }
    emit changed();
}

void HfGatewayModemModel::sendMonitorSetting()
{
    if (m_client->send(m_monitor ? Request::MonitorOn : Request::MonitorOff)) {
        m_monitorMayBeActive = m_monitorMayBeActive || m_monitor;
    } else {
        emit logLine(tr("Could not send the Monitor setting to the modem."), true);
    }
}

void HfGatewayModemModel::clearHeard()
{
    m_heard.clear();
    emit heardChanged();
}

void HfGatewayModemModel::onStateChanged()
{
    m_ready = false;
    m_monitorActive = false;
    m_monitorMayBeActive = false;
    m_listenOffTimeout.stop();
    if (m_client->state() == Client::State::Connected) {
        m_listenOffTimeout.start(kListenOffTimeoutMs);
        if (!m_client->send(Request::ListenOff)) {
            disconnectFromModem();
            emit logLine(tr("Could not send LISTEN OFF. Connection closed."), true);
            return;
        }
    } else if (m_client->state() == Client::State::Disconnected) {
        m_version.clear();
        m_busy = false;
        m_hasSn = false;
        emit logLine(tr("Disconnected"), false);
    }
    emit changed();
}

void HfGatewayModemModel::onReplied(Request request, bool ok, const QString& version)
{
    if (request == Request::ListenOff) {
        m_listenOffTimeout.stop();
        if (!ok) {
            disconnectFromModem();
            emit logLine(tr("The modem refused LISTEN OFF. Connection closed."), true);
            return;
        }
        m_ready = true;
        emit logLine(tr("Connected"), false);
        m_client->send(Request::Version);
        if (m_ready && m_monitor) {
            sendMonitorSetting();
        }
        emit changed();
        return;
    }
    if (!ok) {
        emit logLine(tr("Modem refused %1").arg(ModemHost::toString(request)), true);
        return;
    }
    if (request == Request::Version) {
        m_version = version;
        emit changed();
    } else if (request == Request::MonitorOn || request == Request::MonitorOff) {
        m_monitorActive = request == Request::MonitorOn;
        emit changed();
    }
}

void HfGatewayModemModel::onNotified(const Notification& n)
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
        noteHeard(n.source, QStringLiteral("CQ"), n.bandwidthHz, std::nullopt);
        break;
    case NotificationType::Monitor:
        if (m_monitor && !n.monitorFrom.isEmpty()
            && (n.monitorKind == QLatin1String("CALL") || n.monitorKind == QLatin1String("CQ"))) {
            noteHeard(n.monitorFrom, n.monitorKind, n.bandwidthHz, n.monitorSnDb);
        }
        break;
    case NotificationType::Connected:
        // The external modem owns this session. Sending DISCONNECT or ABORT
        // could make it transmit.
        emit logLine(tr("The modem reports a session %1 to %2 that the gateway did not start")
                         .arg(n.source, n.destination),
                     true);
        break;
    default:
        break;
    }
}

void HfGatewayModemModel::noteHeard(const QString& call, const QString& kind, int bandwidthHz, std::optional<double> snDb)
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
