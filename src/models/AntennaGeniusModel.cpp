#include "core/PeripheralRemovalGuard.h"
#include "AntennaGeniusModel.h"
#include "core/PeripheralAuthCode.h"
#include "core/PeripheralSettings.h"

#include <QUdpSocket>
#include <QTcpSocket>
#include <QTimer>
#include <QNetworkDatagram>
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include <QDebug>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonParseError>

namespace AetherSDR {

// Antenna Genius uses port 9007 for both UDP discovery and TCP control.
static constexpr quint16 kAgPort = 9007;
// Keep-alive interval (seconds).
static constexpr int kKeepAliveMs = 30000;
static constexpr int kSelectionStatusTimeoutMs = 5000;
static constexpr int kPortInitRetryMs = 5000;
// Discovery timeout: remove device if no broadcast in 5 seconds.
static constexpr int kDiscoveryTimeoutMs = 5000;
// Never evict a blocked target: churn must not replenish its retry budget.
// At capacity, unknown targets stay blocked until a tracked budget is reset.
static constexpr qsizetype kMaxAuthTargets = 128;

static QString authTarget(const QString& host, quint16 port)
{
    if (host.trimmed().isEmpty() || port == 0) {
        return {};
    }
    QHostAddress address;
    const QString normalized = address.setAddress(host.trimmed())
        ? address.toString() : host.trimmed().toLower();
    return normalized + QLatin1Char('|') + QString::number(port);
}

static std::optional<int> storedAntenna(const QJsonObject& pair, const QString& key)
{
    if (!pair.contains(key))
        return std::nullopt;
    const auto value = pair.value(key);
    const int antenna = value.toInt(-1);
    if (!value.isDouble() || antenna < 0 || value.toDouble() != antenna) {
        qCWarning(lcTuner) << "AntennaGenius: invalid saved" << key << "antenna";
        return std::nullopt;
    }
    return antenna;
}

// ── AntennaGeniusModel ─────────────────────────────────────────────────────

AntennaGeniusModel::AntennaGeniusModel(QObject* parent)
    : QObject(parent)
{
    m_portA.portId = 1;
    m_portB.portId = 2;
    loadAntennaSettings();

    // Retries every 5s indefinitely until the device returns or the user disconnects.
    // This is intentional for a LAN peripheral that may be power-cycling.
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setObjectName(QStringLiteral("agReconnectTimer"));
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(5000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_deferredShackSwitch) {
            if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::AntennaGenius)) {
                m_reconnectTimer->start();
                return;
            }
            const AgDeviceInfo target = *m_deferredShackSwitch;
            m_deferredShackSwitch.reset();
            if (!m_connected && !isConnecting() && !isAuthBlockedFor(target)) {
                connectToDevice(target);
            }
            return;
        }
        if (!m_connected && !isAuthBlocked() && m_device.port > 0 && reconnectAllowed()
            && (!m_device.ip.isNull() || !m_device.host.isEmpty())) {
            if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::AntennaGenius)) {
                // An AG removal leaves the shared model's ShackSwitch alone.
                // Preserve its single-shot retry while the vault is busy; an
                // explicit disconnect still cancels it by stopping this timer.
                m_reconnectTimer->start();
                return;
            }
            connectToDevice(m_device);
        }
    });

    m_authTimer = new QTimer(this);
    m_authTimer->setSingleShot(true);
    m_authTimer->setInterval(5000);
    connect(m_authTimer, &QTimer::timeout, this, &AntennaGeniusModel::onAuthTimeout);

    // Watchdog for `info get`: falls back to runInitSequence() if the device
    // accepts the TCP connection and sends a prologue but never responds to
    // info get (no body, no terminator). 5 seconds is generous for the real
    // AG (responds in <50ms in pcap traces) while still bounded enough to
    // recover the connection if a device truly stalls.
    m_initWatchdog = new QTimer(this);
    m_initWatchdog->setSingleShot(true);
    m_initWatchdog->setInterval(5000);
    connect(m_initWatchdog, &QTimer::timeout, this, [this]() {
        qCWarning(lcTuner) << "AntennaGenius: info get watchdog fired after 5s --"
                           << "device did not respond, running init anyway";
        m_seqInfo = -1;
        runInitSequence();
    });

    m_portInitWatchdog = new QTimer(this);
    m_portInitWatchdog->setInterval(kPortInitRetryMs);
    connect(m_portInitWatchdog, &QTimer::timeout, this, &AntennaGeniusModel::retryInitialStatus);
}

AntennaGeniusModel::~AntennaGeniusModel()
{
    disconnectFromDevice();
    stopDiscovery();
}

void AntennaGeniusModel::setAuthCode(const QString& code)
{
    m_authCode = code;
    m_userAuthCode = !code.isEmpty();
    m_userAuthEndpoint = m_userAuthCode ? m_attemptEndpoint : QString();
    m_authFailuresByTarget.remove(m_attemptEndpoint);
    if (code.isEmpty() && m_waitingForAuthCode) {
        failAuthentication("Authorization code required");
    }
}

void AntennaGeniusModel::resetAuthBudgetFor(const AgDeviceInfo& info)
{
    const QString host = info.host.isEmpty() ? info.ip.toString() : info.host;
    const QString target = authTarget(host, info.port);
    if (target.isEmpty()) {
        return;
    }
    m_authFailuresByTarget.remove(target);
}

void AntennaGeniusModel::setAuthCodeForAttempt(quint64 attempt, const QString& code,
                                                bool credentialStoreUnavailable)
{
    if (m_waitingForAuthCode && attempt == m_authAttempt) {
        if (code.isEmpty()) {
            // A keychain outage says nothing about the saved code, so
            // auto-reconnect stays available once the keychain returns.
            failAuthentication(credentialStoreUnavailable
                ? "Stored authorization code unavailable" : "Authorization code required",
                !credentialStoreUnavailable);
            return;
        }
        // Restoring the same saved code on a reconnect must not replenish the
        // failure budget. Only an operator-entered code or success resets it.
        m_authCode = code;
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
        m_waitingForAuthCode = false;
        sendAuthentication();
    }
}

bool AntennaGeniusModel::isConnecting() const
{
    return m_tcpSocket && !m_connected
        && m_tcpSocket->state() != QAbstractSocket::UnconnectedState;
}

bool AntennaGeniusModel::isAuthBlockedFor(const QString& host, quint16 port) const
{
    return authBlockedForTarget(authTarget(host, port));
}

bool AntennaGeniusModel::authBlockedForTarget(const QString& target) const
{
    if (target.isEmpty()) {
        return false;
    }
    const auto entry = m_authFailuresByTarget.constFind(target);
    return entry == m_authFailuresByTarget.cend()
        ? m_authFailuresByTarget.size() >= kMaxAuthTargets : entry.value() >= 3;
}

int AntennaGeniusModel::recordAuthFailure()
{
    if (m_attemptEndpoint.isEmpty()) {
        return 0;
    }
    if (!m_authFailuresByTarget.contains(m_attemptEndpoint)
        && m_authFailuresByTarget.size() >= kMaxAuthTargets) {
        return 3;
    }
    return recordPeripheralAuthFailure(m_authFailuresByTarget[m_attemptEndpoint]);
}

bool AntennaGeniusModel::isAuthBlockedFor(const AgDeviceInfo& info) const
{
    return isAuthBlockedFor(info.host.isEmpty() ? info.ip.toString() : info.host, info.port);
}

// ── UDP Discovery ──────────────────────────────────────────────────────────

void AntennaGeniusModel::startDiscovery()
{
    if (m_udpSocket) return;

    m_udpSocket = new QUdpSocket(this);
    // Bind to 9007 to receive AG broadcast datagrams.
    if (!m_udpSocket->bind(QHostAddress::AnyIPv4, kAgPort,
                           QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qCWarning(lcTuner) << "AntennaGenius: failed to bind UDP" << kAgPort
                    << m_udpSocket->errorString() << "— will retry in 5s";
        delete m_udpSocket;
        m_udpSocket = nullptr;
        // Retry bind every 5 seconds until it succeeds
        if (!m_retryTimer) {
            m_retryTimer = new QTimer(this);
            m_retryTimer->setInterval(5000);
            connect(m_retryTimer, &QTimer::timeout, this, [this]() {
                startDiscovery();
                if (m_udpSocket) {
                    // Bind succeeded — stop retrying
                    m_retryTimer->stop();
                }
            });
            m_retryTimer->start();
        }
        return;
    }
    connect(m_udpSocket, &QUdpSocket::readyRead,
            this, &AntennaGeniusModel::onDiscoveryDatagram);

    // Timeout timer: prune devices that stop broadcasting.
    m_discoveryTimeout = new QTimer(this);
    m_discoveryTimeout->setInterval(kDiscoveryTimeoutMs);
    connect(m_discoveryTimeout, &QTimer::timeout, this, []() {
        // In a full implementation we'd track last-seen timestamps.
        // For now we rely on the device broadcasting every 1 s.
    });
    m_discoveryTimeout->start();

    qCDebug(lcTuner) << "AntennaGenius: discovery started on UDP" << kAgPort;
}

void AntennaGeniusModel::stopDiscovery()
{
    if (m_udpSocket) {
        m_udpSocket->close();
        delete m_udpSocket;
        m_udpSocket = nullptr;
    }
    if (m_discoveryTimeout) {
        m_discoveryTimeout->stop();
        delete m_discoveryTimeout;
        m_discoveryTimeout = nullptr;
    }
}

void AntennaGeniusModel::onDiscoveryDatagram()
{
    while (m_udpSocket && m_udpSocket->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_udpSocket->receiveDatagram();
        QString data = QString::fromUtf8(dg.data()).trimmed();

        // AG discovery messages start with "AG "
        if (!data.startsWith("AG "))
            continue;

        // Parse: AG ip=192.168.1.39 port=9007 v=4.0.22 serial=9A-3A-DC name=Ranko_4O3A ...
        auto kvs = parseKeyValues(data.mid(3));  // skip "AG "

        AgDeviceInfo info;
        info.ip       = QHostAddress(kvs.value("ip"));
        info.port     = kvs.value("port", "9007").toUShort();
        info.version  = kvs.value("v");
        info.serial   = kvs.value("serial");
        info.name     = kvs.value("name").replace('_', ' ');
        info.radioPorts   = kvs.value("ports", "2").toInt();
        info.antennaPorts = kvs.value("antennas", "8").toInt();
        info.webPort      = kvs.value("webport", "0").toInt();
        info.mode     = kvs.value("mode");

        if (info.serial.isEmpty() || info.ip.isNull())
            continue;

        // Check if already known
        bool found = false;
        for (auto& d : m_discoveredDevices) {
            if (d.serial == info.serial) {
                d = info;  // refresh
                found = true;
                break;
            }
        }

        if (!found) {
            bool wasEmpty = m_discoveredDevices.isEmpty();
            m_discoveredDevices.append(info);
            qCDebug(lcTuner) << "AntennaGenius: discovered" << info.name
                     << "at" << info.ip.toString() << "serial" << info.serial;
            emit deviceDiscovered(info);
            if (wasEmpty)
                emit presenceChanged(true);
        }

        // Late enrichment: if a UDP beacon arrives after the prologue, update m_device
        // with the full serial/webPort/antennaPorts now that we have them.
        // Runs on every beacon so it works even if the device was already known.
        if (m_connected &&
            (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-")) &&
            !m_device.ip.isNull() && m_device.ip == info.ip) {
            // Only emit antennasChanged once — when the placeholder serial is
            // first replaced by the real one. Emitting on every beacon caused
            // rebuildAntennaButtons to run every ~1s, blanking the display during
            // the brief window before the antenna list response arrived.
            bool serialChanged = (m_device.serial != info.serial);
            m_device.serial       = info.serial;
            m_device.name         = info.name;
            m_device.webPort      = info.webPort;
            m_device.radioPorts   = info.radioPorts;
            m_device.antennaPorts = info.antennaPorts;
            // Re-evaluate Port B visibility from the beacon's authoritative
            // radioPorts value, even if serial did not change.
            emit deviceInfoChanged();
            if (serialChanged)
                emit antennasChanged();
        }
    }
}

// ── TCP Connection ─────────────────────────────────────────────────────────

void AntennaGeniusModel::connectToDevice(const AgDeviceInfo& info)
{
    if (PeripheralRemovalGuard::pending(PeripheralRemovalGuard::Device::AntennaGenius)) {
        // Discovery and radio-connect requests may be one-shot. Keep the latest
        // unrelated ShackSwitch request without changing the live device or
        // reviving the AG being removed. Explicit Disconnect cancels it.
        if (isShackSwitch(info) && !m_connected && !isConnecting()
            && !isAuthBlockedFor(info) && info.port > 0
            && (!info.ip.isNull() || !info.host.isEmpty())) {
            m_deferredShackSwitch = info;
            if (!m_reconnectTimer->isActive()) {
                m_reconnectTimer->start();
            }
        }
        return;
    }
    m_deferredShackSwitch.reset(); // A newer explicit connection supersedes the deferred request.
    const QString host = info.host.isEmpty() ? info.ip.toString() : info.host;
    beginAttemptAt(host, info.port);
    // Always clean up any existing socket — connected or still pending.
    // Multiple auto-connect triggers (radio connect + UDP beacon) can fire
    // in quick succession; without this the R4 accepts the first socket and
    // sends its greeting there while AetherSDR's active socket is a later one.
    if (m_tcpSocket) {
        QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
        m_tcpSocket->abort();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }

    m_device = info;
    if (m_connectTransport) {
        m_connectTransport(host, info.port);
        emit attemptStarted(m_attemptHost);
        return;
    }
    m_tcpSocket = new QTcpSocket(this);
    connect(m_tcpSocket, &QTcpSocket::connected,
            this, &AntennaGeniusModel::onTcpConnected);
    connect(m_tcpSocket, &QTcpSocket::disconnected,
            this, &AntennaGeniusModel::onTcpDisconnected);
    connect(m_tcpSocket, &QTcpSocket::readyRead,
            this, &AntennaGeniusModel::onTcpReadyRead);
    connect(m_tcpSocket, &QTcpSocket::errorOccurred,
            this, [this]() { onTcpError(); });

    qCDebug(lcTuner) << "AntennaGenius: connecting to" << host << ":" << info.port;
    m_tcpSocket->connectToHost(host, info.port);
    emit attemptStarted(m_attemptHost);
}

void AntennaGeniusModel::beginAttemptAt(const QString& host, quint16 port)
{
    m_attemptEndpoint = authTarget(host, port);
    m_attemptHost = host.trimmed();
    if (m_userAuthCode && m_userAuthEndpoint != m_attemptEndpoint) {
        // A code typed for one target must not follow an auto-connect or
        // discovery switch to a different host during reconnect backoff.
        m_authCode.clear();
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
        emit enteredAuthCodeDiscarded();
    }
    beginAttempt();
}

void AntennaGeniusModel::beginAttempt()
{
    m_reconnectTimer->stop();
    if (!m_userAuthCode) {
        m_authCode.clear();
    }
    ++m_authAttempt;
    m_authTimer->stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    m_authCloseReported = false;
    m_gotPrologue = false;
    m_lineBuffer.clear();
    m_nextSeq = 1;
    m_pending.clear();
    m_pendingSelectionA.reset();
    m_pendingSelectionB.reset();
    m_initializingPorts = false;
    m_initialStatusRequired = false;
    m_portInitWatchdog->stop();
    m_antennas.clear();
    m_bands.clear();
    m_portA = AgPortStatus{};
    m_portA.portId = 1;
    m_portB = AgPortStatus{};
    m_portB.portId = 2;
    m_lastRadioBand = 0;
}

void AntennaGeniusModel::onAuthTimeout()
{
    failAuthentication("Authentication timed out", recordAuthFailure() >= 3);
}

void AntennaGeniusModel::connectToAddress(const QHostAddress& ip, quint16 port)
{
    AgDeviceInfo info;
    info.ip   = ip;
    info.port = port;
    info.name = ip.toString();
    info.serial = QString("manual-%1").arg(ip.toString());
    connectToDevice(info);
}

void AntennaGeniusModel::connectToAddress(const QString& host, quint16 port)
{
    AgDeviceInfo info;
    info.host = host;
    info.ip = QHostAddress(host);
    info.port = port;
    info.name = host;
    info.serial = QString("manual-%1").arg(host);
    connectToDevice(info);
}

QString AntennaGeniusModel::peerAddress() const
{
    return m_tcpSocket ? m_tcpSocket->peerAddress().toString() : QString();
}

quint16 AntennaGeniusModel::peerPort() const
{
    return m_tcpSocket ? m_tcpSocket->peerPort() : 0;
}

void AntennaGeniusModel::disconnectFromDevice()
{
    m_deferredShackSwitch.reset();
    m_deliberateDisconnect = true;
    m_authTimer->stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    const bool discardedCode = m_userAuthCode;
    if (discardedCode) {
        m_authCode.clear();
    }
    m_userAuthCode = false;
    m_userAuthEndpoint.clear();
    if (discardedCode) {
        emit enteredAuthCodeDiscarded();
    }
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_keepAlive) {
        m_keepAlive->stop();
        delete m_keepAlive;
        m_keepAlive = nullptr;
    }
    if (m_tcpSocket) {
        QObject::disconnect(m_tcpSocket, nullptr, this, nullptr);
        m_tcpSocket->abort();
        m_tcpSocket->deleteLater();
        m_tcpSocket = nullptr;
    }
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
    m_deliberateDisconnect = false;
    m_antennas.clear();
    m_bands.clear();
    m_portA = AgPortStatus{};
    m_portA.portId = 1;
    m_portB = AgPortStatus{};
    m_portB.portId = 2;
    m_pending.clear();
    m_pendingSelectionA.reset();
    m_pendingSelectionB.reset();
    m_initializingPorts = false;
    m_initialStatusRequired = false;
    m_portInitWatchdog->stop();
}

void AntennaGeniusModel::onTcpConnected()
{
    m_device.ip = m_tcpSocket->peerAddress();
    qCDebug(lcTuner) << "AntennaGenius: TCP connected to" << m_device.ip.toString();

    // Arduino WiFiServer::available() only returns a client once it has sent data.
    // For ShackSwitch (R4) the server speaks first ("V1.0 AG"), so we send an
    // empty line to trigger available() on the R4 side without affecting the
    // protocol (the R4 discards empty lines before the greeting is sent).
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch && m_tcpSocket) {
        m_tcpSocket->write("\r\n");
        m_tcpSocket->flush();
    }

    // Wait for prologue line ("V<version> AG") before sending commands.
}

void AntennaGeniusModel::onTcpDisconnected()
{
    qCDebug(lcTuner) << "AntennaGenius: TCP disconnected";
    const bool rejectedDuringAuth = m_authPending;
    m_authTimer->stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    m_portInitWatchdog->stop();
    m_initializingPorts = false;
    m_initialStatusRequired = false;
    if (rejectedDuringAuth && !m_deliberateDisconnect) {
        const bool blocked = recordAuthFailure() >= 3;
        if (blocked) {
            m_authCode.clear();
            m_userAuthCode = false;
            m_userAuthEndpoint.clear();
        }
        m_authCloseReported = true;
        emit connectionError("Connection closed during authentication");
    }
    bool wasConnected = m_connected;
    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
    if (m_keepAlive) {
        m_keepAlive->stop();
    }
    if (!m_deliberateDisconnect && !isAuthBlocked() && reconnectAllowed()
        && (wasConnected || rejectedDuringAuth)
        && m_device.port > 0 && (!m_device.ip.isNull() || !m_device.host.isEmpty())) {
        m_reconnectTimer->start();
    }
    m_deliberateDisconnect = false;
}

bool AntennaGeniusModel::reconnectAllowed() const
{
    return m_autoReconnect && PeripheralSettings::autoConnect(
        isShackSwitch(m_device) ? QStringLiteral("shackswitch") : QStringLiteral("ag"));
}

void AntennaGeniusModel::onTcpError()
{
    if (!m_tcpSocket) return;
    QString err = m_tcpSocket->errorString();
    qCWarning(lcTuner) << "AntennaGenius: TCP error:" << err;
    if (!m_authPending && !m_authCloseReported) {
        emit connectionError(err);
    }
    // A failed reconnect attempt arrives here (not via onTcpDisconnected) because
    // the socket never reached ConnectedState. Re-arm so we keep retrying until
    // the device returns or the user disconnects. isActive() prevents double-arm
    // when a live drop emits both errorOccurred and disconnected.
    if (!m_deliberateDisconnect && !isAuthBlocked() && !m_authPending && reconnectAllowed() && !m_connected
            && m_device.port > 0 && (!m_device.ip.isNull() || !m_device.host.isEmpty())
            && m_reconnectTimer && !m_reconnectTimer->isActive()) {
        m_reconnectTimer->start();
    }
}

void AntennaGeniusModel::onTcpReadyRead()
{
    if (!m_tcpSocket) return;
    processTcpBytes(m_tcpSocket->readAll());
}

void AntennaGeniusModel::processTcpBytes(const QByteArray& bytes)
{
    m_lineBuffer += QString::fromUtf8(bytes);

    // Process complete lines (terminated by \r\n or \n).
    int pos;
    while ((pos = m_lineBuffer.indexOf('\n')) >= 0) {
        if (pos > kMaxPeripheralLineLength) {
            m_lineBuffer.clear();
            failAuthentication("Antenna Genius protocol line exceeded limit", !m_connected);
            return;
        }
        QString line = m_lineBuffer.left(pos).trimmed();
        m_lineBuffer.remove(0, pos + 1);

        if (line.isEmpty()) continue;

        // First line after connect is prologue: "V<version> AG"
        if (!m_gotPrologue) {
            if (line.startsWith('V') && line.contains("AG")) {
                m_gotPrologue = true;
                // Extract version from "V4.0.22 AG"
                auto parts = line.split(' ');
                if (!parts.isEmpty())
                    m_device.version = parts[0].mid(1);  // skip 'V'
                qCDebug(lcTuner) << "AntennaGenius: prologue received, version"
                         << m_device.version;
                if (line.endsWith(" AUTH")) {
                    qCInfo(lcTuner) << "AntennaGenius: authorization challenge received";
                    if (m_userAuthCode && (m_userAuthEndpoint.isEmpty()
                        || m_userAuthEndpoint != m_attemptEndpoint)) {
                        m_authCode.clear();
                        m_userAuthCode = false;
                        m_userAuthEndpoint.clear();
                    }
                    if (m_authCode.isEmpty()) {
                        m_waitingForAuthCode = true;
                        emit authCodeRequired(m_authAttempt);
                    } else {
                        sendAuthentication();
                    }
                } else {
                    // An unchallenged connection cannot verify a new code.
                    if (m_userAuthCode) {
                        m_authCode.clear();
                        m_userAuthCode = false;
                        m_userAuthEndpoint.clear();
                    }
                    completePrologue();
                }
            }
            continue;
        }

        if (m_waitingForAuthCode) {
            continue;
        }

        if (m_authPending) {
            if (line.startsWith('R')) {
                const QStringList fields = line.split(QLatin1Char('|'));
                bool validResult = false;
                const quint32 result = fields.size() >= 2
                    ? fields.at(1).toUInt(&validResult, 16) : 0;
                // The 4O3A AG API says the echoed sequence is 1 for this
                // command and a zero hex result means success. Also constrain
                // the body: sibling TGXL documents a zero-result Unauthorized
                // rejection. AG firmware responses still need live validation.
                const bool acceptedBody = fields.size() == 2
                    || (fields.size() == 3 && (fields.at(2).isEmpty()
                        || fields.at(2) == QLatin1String("OK")));
                if (fields.size() >= 2 && fields.at(0) == QLatin1String("R1")
                    && validResult && result == 0 && acceptedBody) {
                    m_authTimer->stop();
                    m_authPending = false;
                    const QString acceptedCode = m_userAuthCode ? m_authCode : QString();
                    m_authCode.clear();
                    if (m_userAuthCode) {
                        m_userAuthCode = false;
                        m_userAuthEndpoint.clear();
                        emit authCodeAccepted(acceptedCode);
                    }
                    completePrologue();
                } else {
                    failAuthentication("Authorization code rejected");
                }
            }
            continue;
        }

        processLine(line);
    }
    if (m_lineBuffer.size() > kMaxPeripheralLineLength) {
        m_lineBuffer.clear();
        failAuthentication("Antenna Genius protocol line exceeded limit", !m_connected);
    }
}

void AntennaGeniusModel::completePrologue()
{
    m_authFailuresByTarget.remove(m_attemptEndpoint);
    // If connected via manual IP, enrich from UDP-discovered list.
    if (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-")) {
        for (const AgDeviceInfo& discovered : m_discoveredDevices) {
            if (!discovered.ip.isNull() && discovered.ip == m_device.ip) {
                m_device.serial = discovered.serial;
                m_device.name = discovered.name;
                m_device.webPort = discovered.webPort;
                m_device.radioPorts = discovered.radioPorts;
                m_device.antennaPorts = discovered.antennaPorts;
                break;
            }
        }
        if (m_device.serial.endsWith("-manual") || m_device.serial.startsWith("manual-")) {
            m_device.radioPorts = (m_device.version == "2.0") ? 2 : 1;
        }
    }

    m_initializingPorts = true;
    m_initialStatusRequired = true;
    m_initialPortAReady = false;
    m_initialPortBReady = false;
    m_initialAntennasReady = false;
    m_initialStatusRetryCount = 0;
    m_initializationWarningActive = false;
    m_connected = true;
    emit connected();

    bool wasEmpty = m_discoveredDevices.isEmpty();
    bool found = false;
    for (const AgDeviceInfo& discovered : m_discoveredDevices) {
        if (discovered.serial == m_device.serial) {
            found = true;
            break;
        }
    }
    if (!found) {
        m_discoveredDevices.append(m_device);
        emit deviceDiscovered(m_device);
        if (wasEmpty) {
            emit presenceChanged(true);
        }
    }
    m_seqInfo = sendCommand("info get");
    m_initWatchdog->start();
}

void AntennaGeniusModel::sendAuthentication()
{
    if (m_authCode.isEmpty()) {
        failAuthentication("Authorization code required");
        return;
    }
    if (!validPeripheralAuthCode(m_authCode)) {
        failAuthentication("Invalid authorization code");
        return;
    }
    if (m_userAuthCode && (m_userAuthEndpoint.isEmpty()
        || m_userAuthEndpoint != m_attemptEndpoint)) {
        failAuthentication("Authorization code target changed");
        return;
    }
    // 4O3A Antenna Genius TCPIP API documents this exact AUTH command and a
    // carriage-return terminator. Issue #2313's contributor capture reports
    // LF from the utility and CRLF accepted by AG; use the normal command framing.
    // https://github.com/4o3a/genius-api-docs/wiki/Antenna-Genius-TCPIP-auth
    const QByteArray command = peripheralAuthCommand(PeripheralAuthProtocol::AntennaGenius, m_authCode);
    if (!m_authCommandWriter && !m_tcpSocket) {
        failAuthentication("Connection closed before authorization command");
        return;
    }
    m_authPending = true;
    if (m_authCommandWriter) {
        m_authCommandWriter(command);
    } else {
        m_tcpSocket->write(command);
    }
    m_nextSeq = 2;
    m_authTimer->start();
}

void AntennaGeniusModel::failAuthentication(const QString& reason, bool blockReconnect)
{
    m_authTimer->stop();
    m_authPending = false;
    m_waitingForAuthCode = false;
    if (blockReconnect && !m_attemptEndpoint.isEmpty()
        && (m_authFailuresByTarget.contains(m_attemptEndpoint)
            || m_authFailuresByTarget.size() < kMaxAuthTargets)) {
        m_authFailuresByTarget.insert(m_attemptEndpoint, 3);
    }
    if (blockReconnect) {
        m_authCode.clear();
        m_userAuthCode = false;
        m_userAuthEndpoint.clear();
    }
    m_reconnectTimer->stop();
    m_lineBuffer.clear();
    emit connectionError(reason);
    if (m_tcpSocket) {
        m_tcpSocket->abort();
    }
    if (!isAuthBlocked() && reconnectAllowed() && m_device.port > 0
        && (!m_device.ip.isNull() || !m_device.host.isEmpty())
        && !m_reconnectTimer->isActive()) {
        m_reconnectTimer->start();
    }
}

// ── Command sending ────────────────────────────────────────────────────────

int AntennaGeniusModel::sendCommand(const QString& cmd)
{
    if ((!m_tcpSocket && !m_commandWriter) || !m_connected) return -1;

    int seq = m_nextSeq++;
    if (m_nextSeq > 255) m_nextSeq = 1;

    QString line = QString("C%1|%2\r\n").arg(seq).arg(cmd);
    const QByteArray bytes = line.toUtf8();
    if (m_commandWriter) {
        m_commandWriter(bytes);
    } else {
        m_tcpSocket->write(bytes);
    }

    // Register pending response.
    m_pending[seq] = PendingResponse{cmd, {}};

    return seq;
}

void AntennaGeniusModel::applyDeviceInfo(const QString& line)
{
    auto kvs = parseKeyValues(line);
    if (kvs.contains("ports"))    m_device.radioPorts   = kvs.value("ports").toInt();
    if (kvs.contains("antennas")) m_device.antennaPorts = kvs.value("antennas").toInt();
    if (kvs.contains("serial"))   m_device.serial       = kvs.value("serial");
    if (kvs.contains("name"))     m_device.name         = kvs.value("name").replace('_', ' ');
    if (kvs.contains("v"))        m_device.version      = kvs.value("v");
    if (kvs.contains("mode"))     m_device.mode         = kvs.value("mode");
}

// ── Line processing ────────────────────────────────────────────────────────

void AntennaGeniusModel::processLine(const QString& line)
{
    if (line.isEmpty()) return;

    QChar prefix = line[0];

    if (prefix == 'R') {
        // Response: R<seq>|<hex_code>|<body>
        // Find first and second '|'.
        int p1 = line.indexOf('|');
        int p2 = (p1 >= 0) ? line.indexOf('|', p1 + 1) : -1;
        if (p1 < 0 || p2 < 0) return;

        int seq = line.mid(1, p1 - 1).toInt();
        int code = line.mid(p1 + 1, p2 - p1 - 1).toInt(nullptr, 16);
        QString body = line.mid(p2 + 1);

        processResponse(seq, code, body);

    } else if (prefix == 'S') {
        // Status: S0|<body>
        int p1 = line.indexOf('|');
        if (p1 < 0) return;
        QString body = line.mid(p1 + 1);
        processStatus(body);
    }
}

void AntennaGeniusModel::processResponse(int seq, int code, const QString& body)
{
    if (!m_pending.contains(seq)) return;

    auto& pr = m_pending[seq];
    if (code != 0) {
        qCWarning(lcTuner) << "AntennaGenius: command" << pr.command
                           << "error code" << Qt::hex << code << body;
        for (int portId = 1; portId <= 2; ++portId) {
            auto& selection = portId == 1 ? m_pendingSelectionA : m_pendingSelectionB;
            if (selection && selection->sequence == seq) {
                selection.reset();
                emit portStatusChanged(portId);
            }
        }
        if (pr.command.startsWith(QStringLiteral("port set "))) {
            m_pending.remove(seq);
            return;
        }
    }

    // Empty body marks end of multi-line response.
    if (body.isEmpty()) {
        // Process accumulated lines.
        QStringList lines = pr.lines;
        QString cmd = pr.command;
        m_pending.remove(seq);

        if (seq == m_seqInfo) {
            // info get's body is normally consumed inline in the accumulate
            // block below. This terminator path handles error responses
            // (code != 0) and defensive parsing if a firmware variant sends
            // a terminator after the body line. In either case, proceed to
            // init so the connection does not stall.
            if (!lines.isEmpty()) {
                applyDeviceInfo(lines.first());
                emit deviceInfoChanged();
            } else if (code != 0) {
                qCWarning(lcTuner) << "AntennaGenius: info get error code 0x"
                                   << Qt::hex << code
                                   << ", keeping heuristic radioPorts="
                                   << m_device.radioPorts;
            }
            m_seqInfo = -1;        // clear so a wrapped-seq reuse cannot re-enter the info-get path
            m_initWatchdog->stop();  // device answered (even with error) before watchdog could fire
            runInitSequence();
            return;
        }

        if (seq == m_seqAntennaList) {
            // Parse antenna list.
            m_antennas.clear();
            for (const QString& l : lines) {
                // "antenna 1 name=Antenna_1 tx=0000 rx=0001 inband=0000"
                static QRegularExpression reId(R"(antenna\s+(\d+)\s+)");
                auto mId = reId.match(l);
                if (!mId.hasMatch()) continue;

                AgAntennaInfo ant;
                ant.id = mId.captured(1).toInt();
                auto kvs = parseKeyValues(l.mid(mId.capturedEnd()));
                ant.name = kvs.value("name").replace('_', ' ');
                ant.txBandMask  = kvs.value("tx", "0").toUShort(nullptr, 16);
                ant.rxBandMask  = kvs.value("rx", "0").toUShort(nullptr, 16);
                ant.inbandMask  = kvs.value("inband", "0").toUShort(nullptr, 16);
                m_antennas.append(ant);
            }
            qCDebug(lcTuner) << "AntennaGenius:" << m_antennas.size() << "antennas loaded";
            if (m_initializingPorts || m_portInitWatchdog->isActive()) {
                m_initialAntennasReady = !m_antennas.isEmpty();
            }
            emit antennasChanged();
            finishInitialPortStatus();

        } else if (seq == m_seqBandList) {
            // Parse band list.
            m_bands.clear();
            for (const QString& l : lines) {
                // "band 0 name=None freq_start=0.000000 freq_stop=0.000000"
                static QRegularExpression reId(R"(band\s+(\d+)\s+)");
                auto mId = reId.match(l);
                if (!mId.hasMatch()) continue;

                AgBandInfo band;
                band.id = mId.captured(1).toInt();
                auto kvs = parseKeyValues(l.mid(mId.capturedEnd()));
                band.name = kvs.value("name").replace('_', ' ');
                band.freqStartMhz = kvs.value("freq_start", "0").toDouble();
                band.freqStopMhz  = kvs.value("freq_stop", "0").toDouble();
                m_bands.append(band);
            }
            qCDebug(lcTuner) << "AntennaGenius:" << m_bands.size() << "bands loaded";
            emit bandsChanged();

            // Bands just loaded — reprocess the cached radio frequency so
            // m_lastRadioBand is set correctly for antenna save/recall.
            if (m_lastRadioFreqMhz > 0.0) {
                qCDebug(lcTuner) << "AG-FREQ: reprocessing cached frequency"
                         << m_lastRadioFreqMhz << "MHz after bands loaded";
                double cached = m_lastRadioFreqMhz;
                m_lastRadioFreqMhz = 0.0;  // prevent infinite loop
                setRadioFrequency(cached);
            }

        } else if (seq == m_seqPortA || seq == m_seqPortB) {
            // Parse port status from response (single line).
            const int portId = seq == m_seqPortA ? 1 : 2;
            if (!lines.isEmpty() && hasCompletePortStatus(lines.first(), portId)) {
                auto ps = parsePortStatus(lines.first());
                if (seq == m_seqPortA) {
                    if (!m_initialPortAReady) {
                        m_portA = ps;
                        m_prevBandA = ps.band;
                        emit portStatusChanged(1);
                    }
                    m_initialPortAReady = true;
                } else {
                    if (!m_initialPortBReady) {
                        m_portB = ps;
                        m_prevBandB = ps.band;
                        emit portStatusChanged(2);
                    }
                    m_initialPortBReady = true;
                }
                finishInitialPortStatus();
            } else {
                qCWarning(lcTuner) << "AntennaGenius: incomplete initial status for port" << portId;
            }
        }
        return;
    }

    // Accumulate multi-line response body.
    if (code == 0) {
        if (seq == m_seqInfo) {
            // info get is single-shot: body arrives on one non-empty line
            // with no terminating empty body. Parse inline rather than
            // accumulating, and proceed to init.
            applyDeviceInfo(body);
            emit deviceInfoChanged();
            m_pending.remove(seq);
            m_seqInfo = -1;        // clear so a wrapped-seq reuse cannot re-enter the info-get path
            m_initWatchdog->stop();  // info get answered before watchdog could fire
            runInitSequence();
        } else {
            pr.lines.append(body);
        }
    }
}

void AntennaGeniusModel::processStatus(const QString& body)
{
    // "port 1 auto=1 source=AUTO band=5 rxant=3 txant=3 tx=0 inhibit=0"
    if (body.startsWith("port ")) {
        auto ps = parsePortStatus(body);
        if (ps.portId != 1 && ps.portId != 2) {
            qCWarning(lcTuner) << "AntennaGenius: invalid port status:" << body;
            return;
        }
        const auto fields = parseKeyValues(body);
        for (const QString& key : {QStringLiteral("rxant"), QStringLiteral("txant")}) {
            if (fields.contains(key)) {
                bool ok = false;
                const int antenna = fields.value(key).toInt(&ok);
                if (!ok || antenna < 0) {
                    qCWarning(lcTuner) << "AntennaGenius: invalid antenna in port status:" << body;
                    return;
                }
            }
        }
        const bool complete = hasCompletePortStatus(body, ps.portId);
        const auto& previous = ps.portId == 1 ? m_portA : m_portB;
        if (!fields.contains("auto")) ps.autoMode = previous.autoMode;
        if (!fields.contains("source")) ps.source = previous.source;
        if (!fields.contains("band")) ps.band = previous.band;
        if (!fields.contains("rxant")) ps.rxAntenna = previous.rxAntenna;
        if (!fields.contains("txant")) ps.txAntenna = previous.txAntenna;
        if (!fields.contains("tx")) ps.transmitting = previous.transmitting;
        if (!fields.contains("inhibit")) ps.inhibited = previous.inhibited;
        bool& initialReady = ps.portId == 1 ? m_initialPortAReady : m_initialPortBReady;
        const bool firstStatus = m_initialStatusRequired && !initialReady;
        if (complete)
            initialReady = true;
        auto& pending = ps.portId == 1 ? m_pendingSelectionA : m_pendingSelectionB;
        const int reportedBand = ps.band > 0 ? ps.band
            : (ps.portId == 1 ? m_lastRadioBand : 0);
        if ((ps.portId == 1 || ps.portId == 2) && pending
            && reportedBand > 0 && pending->band > 0 && reportedBand != pending->band)
            pending.reset();
        if (complete && pending
            && pending->rxAntenna == ps.rxAntenna && pending->txAntenna == ps.txAntenna) {
            if (pending->band > 0 && reportedBand == pending->band) {
                if (pending->clearBandMemory && (pending->saveRx || pending->saveTx))
                    setBandSettings(ps.portId, pending->band,
                        pending->saveRx ? std::optional<int>(ps.rxAntenna) : std::nullopt,
                        pending->saveTx ? std::optional<int>(ps.txAntenna) : std::nullopt, true);
                else if (pending->clearBandMemory)
                    clearBandSettings(ps.portId, pending->band);
                else if (pending->saveRx || pending->saveTx)
                    setBandSettings(ps.portId, pending->band,
                        pending->saveRx ? std::optional<int>(ps.rxAntenna) : std::nullopt,
                        pending->saveTx ? std::optional<int>(ps.txAntenna) : std::nullopt);
            }
            pending.reset();
        }
        if (ps.portId == 1) {
            int oldBand = m_portA.band;
            int oldRxAnt = m_portA.rxAntenna;
            int oldTxAnt = m_portA.txAntenna;
            m_portA = ps;
            emit portStatusChanged(1);
            if (!firstStatus && ps.band != oldBand)
                onBandChanged(1, oldBand, oldRxAnt, oldTxAnt, ps.band);
        } else if (ps.portId == 2) {
            int oldBand = m_portB.band;
            int oldRxAnt = m_portB.rxAntenna;
            int oldTxAnt = m_portB.txAntenna;
            m_portB = ps;
            emit portStatusChanged(2);
            if (!firstStatus && ps.band != oldBand)
                onBandChanged(2, oldBand, oldRxAnt, oldTxAnt, ps.band);
        }
        finishInitialPortStatus();
    } else if (body.startsWith("antenna reload")) {
        // Antenna configuration changed on the device — re-fetch.
        qCDebug(lcTuner) << "AntennaGenius: antenna reload, re-fetching list";
        m_seqAntennaList = sendCommand("antenna list");
    }
    // "relay tx=00 rx=04 state=04" — informational, ignore for now.
}

// ── Init sequence ──────────────────────────────────────────────────────────

void AntennaGeniusModel::runInitSequence()
{
    m_initializingPorts = true;
    m_initialStatusRequired = true;
    m_initialPortAReady = false;
    m_initialPortBReady = false;
    m_initialAntennasReady = false;
    m_initialStatusRetryCount = 0;
    m_initializationWarningActive = false;
    m_seqAntennaList = sendCommand("antenna list");
    m_seqBandList    = sendCommand("band list");
    m_seqPortA       = sendCommand("port get 1");
    m_seqPortB       = sendCommand("port get 2");
    m_seqSubPort     = sendCommand("sub port all");
    m_seqSubRelay    = sendCommand("sub relay");
    m_portInitWatchdog->start();

    // Start keep-alive timer.
    if (!m_keepAlive) {
        m_keepAlive = new QTimer(this);
        connect(m_keepAlive, &QTimer::timeout,
                this, &AntennaGeniusModel::onKeepAlive);
    }
    m_keepAlive->start(kKeepAliveMs);
}

void AntennaGeniusModel::finishInitialPortStatus()
{
    if (!m_initialStatusRequired || !m_initialPortAReady || !m_initialAntennasReady
        || (m_device.radioPorts > 1 && !m_initialPortBReady))
        return;
    m_portInitWatchdog->stop();
    if (m_initializationWarningActive) {
        m_initializationWarningActive = false;
        emit initializationWarning(QString());
    }
    if (!m_initializingPorts) {
        return;
    }
    m_initializingPorts = false;
    if (isShackSwitch(m_device))
        return;
    for (int portId = 1; portId <= m_device.radioPorts && portId <= 2; ++portId) {
        const int band = effectiveBand(portId);
        if (band > 0)
            recallBandAntennas(portId, band);
    }
}

void AntennaGeniusModel::retryInitialStatus()
{
    if (!m_connected) {
        m_portInitWatchdog->stop();
        return;
    }
    if (m_initializingPorts) {
        m_initializingPorts = false;
        qCWarning(lcTuner) << "AntennaGenius: initial antenna or port status incomplete; retrying without automatic recall";
        m_initializationWarningActive = true;
        emit initializationWarning(tr("Antenna or port status incomplete; retrying"));
    }
    if (m_initialStatusRetryCount >= m_initialStatusRetryLimit) {
        m_portInitWatchdog->stop();
        qCWarning(lcTuner) << "AntennaGenius: initial status retry limit reached";
        m_initializationWarningActive = true;
        emit initializationWarning(tr("Antenna or port status unavailable; reconnect to retry"));
        return;
    }
    ++m_initialStatusRetryCount;
    if (!m_initialAntennasReady) {
        m_pending.remove(m_seqAntennaList);
        m_seqAntennaList = sendCommand("antenna list");
    }
    if (!m_initialPortAReady) {
        m_pending.remove(m_seqPortA);
        m_seqPortA = sendCommand("port get 1");
    }
    if (m_device.radioPorts > 1 && !m_initialPortBReady) {
        m_pending.remove(m_seqPortB);
        m_seqPortB = sendCommand("port get 2");
    }
}

bool AntennaGeniusModel::hasCompletePortStatus(const QString& line, int portId) const
{
    if (parsePortStatus(line).portId != portId)
        return false;
    const auto fields = parseKeyValues(line);
    bool rxOk = false;
    bool txOk = false;
    const int rx = fields.value("rxant").toInt(&rxOk);
    const int tx = fields.value("txant").toInt(&txOk);
    return rxOk && txOk && rx >= 0 && tx >= 0;
}

void AntennaGeniusModel::onKeepAlive()
{
    if (m_connected)
        sendCommand("ping");
}

// ── Commands ───────────────────────────────────────────────────────────────

void AntennaGeniusModel::selectAntenna(int portId, int antennaId)
{
    const auto ps = selectionState(portId);
    const int band = effectiveBand(portId);
    int txAnt = ps.txAntenna;  // keep current TX antenna by default
    if (antennaId == 0) {
        txAnt = 0;
    } else if (canTxOnBand(antennaId, band)) {
        txAnt = antennaId;
    } else {
        qCDebug(lcTuner) << "AntennaGenius: antenna" << antennaName(antennaId)
                 << "has no TX permission on band" << bandName(band)
                 << "— keeping txant=" << txAnt;
    }

    selectAntennaPair(portId, antennaId, txAnt, antennaId != 0,
                      antennaId != 0, antennaId == 0);
}

void AntennaGeniusModel::selectRxAntenna(int portId, int antennaId)
{
    const int band = effectiveBand(portId);
    if (antennaId > 0 && band > 0 && !canRxOnBand(antennaId, band)) {
        qCDebug(lcTuner) << "AntennaGenius: antenna" << antennaName(antennaId)
                 << "has no RX permission on band" << bandName(band) << "— blocked";
        return;
    }
    const auto ps = selectionState(portId);
    selectAntennaPair(portId, antennaId, ps.txAntenna, true, false);
}

void AntennaGeniusModel::selectTxAntenna(int portId, int antennaId)
{
    const int band = effectiveBand(portId);
    if (antennaId > 0 && !canTxOnBand(antennaId, band)) {
        qCDebug(lcTuner) << "AntennaGenius: antenna" << antennaName(antennaId)
                 << "has no TX permission on band" << bandName(band) << "— blocked";
        return;
    }
    const auto ps = selectionState(portId);
    selectAntennaPair(portId, ps.rxAntenna, antennaId, false, true);
}

bool AntennaGeniusModel::antennaInUseByOtherPort(int portId, int antennaId) const
{
    if (antennaId <= 0 || isShackSwitch(m_device))
        return false;
    const int otherId = portId == 1 ? 2 : 1;
    const auto& reported = otherId == 1 ? m_portA : m_portB;
    const auto requested = selectionState(otherId);
    return reported.rxAntenna == antennaId || reported.txAntenna == antennaId
        || requested.rxAntenna == antennaId || requested.txAntenna == antennaId;
}

void AntennaGeniusModel::selectAntennaPair(int portId, int rxAntenna, int txAntenna,
                                           bool saveRx, bool saveTx, bool clearBandMemory)
{
    if (!m_connected || (portId != 1 && portId != 2)) return;
    if (m_initializingPorts) {
        qCWarning(lcTuner) << "AntennaGenius: antenna selection blocked until initial port status is available";
        return;
    }
    const auto current = selectionState(portId);
    if (m_initialStatusRequired) {
        const bool ownReady = portId == 1 ? m_initialPortAReady : m_initialPortBReady;
        const bool otherReady = isShackSwitch(m_device) || m_device.radioPorts <= 1
            || (portId == 1 ? m_initialPortBReady : m_initialPortAReady);
        const bool picksAntenna = (rxAntenna > 0 && rxAntenna != current.rxAntenna)
            || (txAntenna > 0 && txAntenna != current.txAntenna);
        if (!ownReady || (!otherReady && picksAntenna)) {
            qCWarning(lcTuner) << "AntennaGenius: antenna selection blocked until port status is available";
            m_initializationWarningActive = true;
            emit initializationWarning(tr("Port status incomplete; antenna selection is limited while retrying"));
            return;
        }
    }
    if ((rxAntenna != current.rxAntenna && antennaInUseByOtherPort(portId, rxAntenna))
        || (txAntenna != current.txAntenna && antennaInUseByOtherPort(portId, txAntenna))) {
        qCDebug(lcTuner) << "AntennaGenius: RX antenna" << antennaName(rxAntenna)
                 << "or TX antenna" << antennaName(txAntenna)
                 << "is already in use on the other port — blocked";
        return;
    }

    QString cmd = QString("port set %1 rxant=%2 txant=%3")
                      .arg(portId).arg(rxAntenna).arg(txAntenna);
    qCDebug(lcTuner) << "AntennaGenius:" << cmd;
    auto& pending = portId == 1 ? m_pendingSelectionA : m_pendingSelectionB;
    const int band = effectiveBand(portId);
    const bool carryRx = pending && pending->band == band && pending->saveRx;
    const bool carryTx = pending && pending->band == band && pending->saveTx;
    const bool carryClear = pending && pending->band == band && pending->clearBandMemory;
    const int sequence = sendCommand(cmd);
    if (sequence < 0) {
        qCWarning(lcTuner) << "AntennaGenius: command could not be sent:"
                           << cmd;
        return;
    }

    const quint64 generation = ++m_selectionGeneration;
    pending = PendingSelection{sequence, generation, rxAntenna, txAntenna, band,
                               saveRx || carryRx, saveTx || carryTx,
                               clearBandMemory || carryClear};
    QTimer::singleShot(kSelectionStatusTimeoutMs, this, [this, portId, generation]() {
        auto& selection = portId == 1 ? m_pendingSelectionA : m_pendingSelectionB;
        if (selection && selection->generation == generation) {
            qCWarning(lcTuner) << "AntennaGenius: no matching status for antenna selection on port" << portId;
            selection.reset();
            emit portStatusChanged(portId);
        }
    });
    if (band <= 0) {
        qCDebug(lcTuner) << "AntennaGenius: no band known for port" << portId
                 << "— cannot save antenna mappings";
    }
}

AgPortStatus AntennaGeniusModel::selectionState(int portId) const
{
    auto status = portId == 1 ? m_portA : m_portB;
    const auto& pending = portId == 1 ? m_pendingSelectionA : m_pendingSelectionB;
    if (pending) {
        status.rxAntenna = pending->rxAntenna;
        status.txAntenna = pending->txAntenna;
    }
    return status;
}

void AntennaGeniusModel::setAutoMode(int portId, bool on)
{
    if (!m_connected) return;
    QString cmd = QString("port set %1 auto=%2")
                      .arg(portId).arg(on ? 1 : 0);
    qCDebug(lcTuner) << "AntennaGenius:" << cmd;
    sendCommand(cmd);
}

void AntennaGeniusModel::setRadioFrequency(double freqMhz)
{
    // Always cache the frequency so we can reprocess when bands load later.
    m_lastRadioFreqMhz = freqMhz;

    if (!m_connected) {
        return;
    }
    if (m_bands.isEmpty()) {
        qCDebug(lcTuner) << "AG-FREQ: bands list empty, will reprocess when loaded";
        return;
    }

    // Find which AG band this frequency falls in.
    int matchedBand = 0;
    for (const auto& b : m_bands) {
        if (b.id == 0) continue;
        if (freqMhz >= b.freqStartMhz && freqMhz <= b.freqStopMhz) {
            matchedBand = b.id;
            break;
        }
    }

    if (matchedBand == m_lastRadioBand) return;  // no change

    // If the AG itself reports bands (BCD connection), defer save/recall
    // to onBandChanged() which uses the correctly-captured old antenna.
    if (m_portA.band > 0) {
        m_lastRadioBand = matchedBand;
        qCDebug(lcTuner) << "AG-FREQ:" << freqMhz << "MHz → band"
                 << matchedBand << bandName(matchedBand)
                 << "(AG has BCD, deferring to onBandChanged)";
        emit radioBandChanged(matchedBand);
        return;
    }

    int oldBand = m_lastRadioBand;
    m_lastRadioBand = matchedBand;

    qCDebug(lcTuner) << "AG-FREQ:" << freqMhz << "MHz → band"
             << matchedBand << bandName(matchedBand)
             << "(was" << oldBand << bandName(oldBand) << ")";

    emit radioBandChanged(matchedBand);
    if (m_initializingPorts)
        return;

    // Save current antennas for old band, but only if no user selection
    // already exists.  selectAntenna() is the authoritative save point
    // for manual choices — here we just capture the default if the user
    // never explicitly picked an antenna for the old band.
    if (oldBand > 0) {
        if (!recallBandRxAntenna(1, oldBand) && m_portA.rxAntenna > 0)
            saveBandRxAntenna(1, oldBand, m_portA.rxAntenna);
        if (!recallBandTxAntenna(1, oldBand) && m_portA.txAntenna > 0)
            saveBandTxAntenna(1, oldBand, m_portA.txAntenna);
    }

    if (matchedBand == 0) return;

    // ShackSwitch manages its own band→antenna mapping — don't override it.
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch) {
        qCDebug(lcTuner) << "AG-FREQ: ShackSwitch connected — skipping auto-recall";
        return;
    }

    const auto savedRx = recallBandRxAntenna(1, matchedBand);
    const auto savedTx = recallBandTxAntenna(1, matchedBand);
    if (savedRx || savedTx) {
        recallBandAntennas(1, matchedBand);
    } else {
        // First visit to this band — save current antennas as defaults.
        if (m_portA.rxAntenna > 0)
            saveBandRxAntenna(1, matchedBand, m_portA.rxAntenna);
        if (m_portA.txAntenna > 0)
            saveBandTxAntenna(1, matchedBand, m_portA.txAntenna);
    }
}

// ── Band-specific TX/RX antenna memory (AppSettings persistence) ──────────

void AntennaGeniusModel::loadAntennaSettings()
{
    auto& s = AppSettings::instance();
    const QString key = QStringLiteral("AntennaGenius");
    if (s.contains(key)) {
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(s.value(key).toString().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()
            || doc.object().value("version").toInt() != 1) {
            qCWarning(lcTuner) << "AntennaGenius: invalid or unsupported settings; not overwriting";
            m_antennaSettingsWritable = false;
            return;
        }
        m_antennaSettings = doc.object();
        return;
    }

    m_antennaSettings = QJsonObject{{"version", 1},
        {"splitRxTx", false}};
    QJsonObject ports;
    QStringList migratedKeys;
    for (int port = 1; port <= 2; ++port) {
        QJsonObject bands;
        for (int band = 1; band <= 15; ++band) {
            const QString legacy = QString("AG_Port%1_Band%2").arg(port).arg(band);
            QJsonObject pair;
            bool legacyOk = false;
            const int old = s.value(legacy).toInt(&legacyOk);
            if (old > 0)
                pair["rx"] = old;
            if (old > 0)
                pair["legacyTx"] = old;
            if (s.contains(legacy) && !legacyOk)
                qCWarning(lcTuner) << "AntennaGenius: ignoring invalid legacy antenna settings for" << legacy;
            if (!pair.isEmpty())
                bands[QString::number(band)] = pair;
            migratedKeys << legacy;
        }
        if (!bands.isEmpty())
            ports[QString::number(port)] = bands;
    }
    m_antennaSettings["ports"] = ports;
    s.setValue(key, QString::fromUtf8(QJsonDocument(m_antennaSettings).toJson(QJsonDocument::Compact)));
    for (const auto& oldKey : migratedKeys)
        s.remove(oldKey);
    s.save();
}

void AntennaGeniusModel::saveAntennaSettings()
{
    if (!m_antennaSettingsWritable) {
        qCWarning(lcTuner) << "AntennaGenius: settings change could not be saved";
        return;
    }
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("AntennaGenius"),
               QString::fromUtf8(QJsonDocument(m_antennaSettings).toJson(QJsonDocument::Compact)));
    s.save();
}

bool AntennaGeniusModel::splitRxTxMode() const
{
    return m_antennaSettings.value("splitRxTx").toBool();
}

void AntennaGeniusModel::setSplitRxTxMode(bool split)
{
    m_antennaSettings["splitRxTx"] = split;
    saveAntennaSettings();
}

QJsonObject AntennaGeniusModel::bandSettings(int portId, int bandId) const
{
    return m_antennaSettings.value("ports").toObject()
        .value(QString::number(portId)).toObject().value(QString::number(bandId)).toObject();
}

void AntennaGeniusModel::setBandSettings(int portId, int bandId,
                                          std::optional<int> rx, std::optional<int> tx,
                                          bool clearExisting)
{
    auto ports = m_antennaSettings.value("ports").toObject();
    auto bands = ports.value(QString::number(portId)).toObject();
    auto pair = clearExisting ? QJsonObject()
                              : bands.value(QString::number(bandId)).toObject();
    if (rx)
        pair["rx"] = *rx;
    if (tx) {
        pair["tx"] = *tx;
        pair.remove(QStringLiteral("legacyTx"));
    }
    bands[QString::number(bandId)] = pair;
    ports[QString::number(portId)] = bands;
    m_antennaSettings["ports"] = ports;
    saveAntennaSettings();
}

void AntennaGeniusModel::clearBandSettings(int portId, int bandId)
{
    auto ports = m_antennaSettings.value("ports").toObject();
    auto bands = ports.value(QString::number(portId)).toObject();
    bands.remove(QString::number(bandId));
    if (bands.isEmpty())
        ports.remove(QString::number(portId));
    else
        ports[QString::number(portId)] = bands;
    m_antennaSettings["ports"] = ports;
    saveAntennaSettings();
}

void AntennaGeniusModel::saveBandRxAntenna(int portId, int bandId, int antennaId)
{
    setBandSettings(portId, bandId, antennaId, std::nullopt);
}

void AntennaGeniusModel::saveBandTxAntenna(int portId, int bandId, int antennaId)
{
    setBandSettings(portId, bandId, std::nullopt, antennaId);
}

std::optional<int> AntennaGeniusModel::recallBandRxAntenna(int portId, int bandId) const
{
    const auto pair = bandSettings(portId, bandId);
    return storedAntenna(pair, QStringLiteral("rx"));
}

std::optional<int> AntennaGeniusModel::recallBandTxAntenna(int portId, int bandId) const
{
    const auto pair = bandSettings(portId, bandId);
    if (pair.contains("tx"))
        return storedAntenna(pair, QStringLiteral("tx"));
    const auto legacy = storedAntenna(pair, QStringLiteral("legacyTx"));
    if (legacy && *legacy > 0 && canTxOnBand(*legacy, bandId))
        return *legacy;
    return std::nullopt;
}

void AntennaGeniusModel::recallBandAntennas(int portId, int bandId)
{
    if (m_initializingPorts || (m_initialStatusRequired
        && (!m_initialAntennasReady || !m_initialPortAReady
            || (m_device.radioPorts > 1 && !m_initialPortBReady))))
        return;
    const auto savedRx = recallBandRxAntenna(portId, bandId);
    const auto savedTx = recallBandTxAntenna(portId, bandId);
    if (!savedRx && !savedTx)
        return;

    const auto& ps = (portId == 1) ? m_portA : m_portB;
    int rxAntenna = savedRx.value_or(ps.rxAntenna);
    int txAntenna = savedTx.value_or(ps.txAntenna);
    if (savedRx && rxAntenna > 0 && !canRxOnBand(rxAntenna, bandId))
        rxAntenna = ps.rxAntenna;
    if (savedTx && txAntenna > 0 && !canTxOnBand(txAntenna, bandId))
        txAntenna = ps.txAntenna;

    qCDebug(lcTuner) << "AntennaGenius: recalling RX" << antennaName(rxAntenna)
             << "TX" << antennaName(txAntenna) << "for band"
             << bandName(bandId) << "on port" << portId;
    selectAntennaPair(portId, rxAntenna, txAntenna, false, false);
}

void AntennaGeniusModel::onBandChanged(int portId, int oldBand,
                                       int oldRxAnt, int oldTxAnt, int newBand)
{
    if (m_initializingPorts)
        return;
    qCDebug(lcTuner) << "AntennaGenius: port" << portId << "band changed"
             << bandName(oldBand) << "→" << bandName(newBand);

    // ShackSwitch manages its own band→antenna mapping — don't override it.
    const bool isShackSwitch = m_device.name.contains("ShackSwitch", Qt::CaseInsensitive);
    if (isShackSwitch) {
        qCDebug(lcTuner) << "AntennaGenius: ShackSwitch connected — skipping band recall for port" << portId;
        return;
    }

    // Save the antennas for the old band, but only as defaults if no
    // user selection already exists.  Selection methods are authoritative
    // save point — we must not overwrite a manual choice with whatever the
    // AG's auto-mode happened to leave active.
    if (oldBand > 0) {
        if (oldRxAnt > 0 && !recallBandRxAntenna(portId, oldBand))
            saveBandRxAntenna(portId, oldBand, oldRxAnt);
        if (oldTxAnt > 0 && !recallBandTxAntenna(portId, oldBand))
            saveBandTxAntenna(portId, oldBand, oldTxAnt);
    }

    if (newBand == 0) return;  // band "None" — nothing to recall

    // Recall the saved antennas for the new band.
    // Always send the command even if the AG already reports the correct
    // antenna — the AG's auto-mode can override it milliseconds later,
    // so we must assert our preference unconditionally (#1213).
    const auto savedRx = recallBandRxAntenna(portId, newBand);
    const auto savedTx = recallBandTxAntenna(portId, newBand);
    if (savedRx || savedTx) {
        recallBandAntennas(portId, newBand);
    } else if (newBand > 0) {
        // First visit to this band — save current antennas as defaults.
        const auto& ps = (portId == 1) ? m_portA : m_portB;
        if (ps.rxAntenna > 0)
            saveBandRxAntenna(portId, newBand, ps.rxAntenna);
        if (ps.txAntenna > 0)
            saveBandTxAntenna(portId, newBand, ps.txAntenna);
    }
}

// ── TX/RX band permission checks ──────────────────────────────────────────

int AntennaGeniusModel::effectiveBand(int portId) const
{
    const auto& ps = (portId == 1) ? m_portA : m_portB;
    int band = ps.band;
    if (band <= 0 && portId == 1)
        band = m_lastRadioBand;
    return band;
}

bool AntennaGeniusModel::canTxOnBand(int antennaId, int bandId) const
{
    if (m_initialStatusRequired && !m_initialAntennasReady) return false;
    if (bandId <= 0 || bandId > 15) return false;
    for (const auto& a : m_antennas) {
        if (a.id == antennaId)
            return (a.txBandMask >> bandId) & 1;
    }
    return false;
}

bool AntennaGeniusModel::canRxOnBand(int antennaId, int bandId) const
{
    if (m_initialStatusRequired && !m_initialAntennasReady) return false;
    if (bandId <= 0 || bandId > 15) return false;
    for (const auto& a : m_antennas) {
        if (a.id == antennaId)
            return (a.rxBandMask >> bandId) & 1;
    }
    return false;
}

bool AntennaGeniusModel::isShackSwitch(const AgDeviceInfo& info)
{
    return info.name.contains(QStringLiteral("ShackSwitch"), Qt::CaseInsensitive);
}

// ── Helpers ────────────────────────────────────────────────────────────────

QString AntennaGeniusModel::bandName(int bandId) const
{
    for (const auto& b : m_bands) {
        if (b.id == bandId) return b.name;
    }
    return bandId == 0 ? "None" : QString::number(bandId);
}

QString AntennaGeniusModel::antennaName(int antennaId) const
{
    for (const auto& a : m_antennas) {
        if (a.id == antennaId) return a.name;
    }
    return antennaId == 0 ? "None" : QString("Ant %1").arg(antennaId);
}

QMap<QString, QString> AntennaGeniusModel::parseKeyValues(const QString& text)
{
    QMap<QString, QString> result;
    // Match key=value pairs (value may contain no spaces, or be quoted).
    static QRegularExpression re(R"((\w+)=(\S+))");
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        result[m.captured(1)] = m.captured(2);
    }
    return result;
}

AgPortStatus AntennaGeniusModel::parsePortStatus(const QString& text) const
{
    AgPortStatus ps;
    // Extract port ID: "port 1 ..."
    static QRegularExpression rePort(R"(port\s+(\d+)\s+)");
    auto m = rePort.match(text);
    if (m.hasMatch()) {
        ps.portId = m.captured(1).toInt();
    }

    auto kvs = parseKeyValues(text);
    ps.autoMode     = kvs.value("auto", "1") == "1";
    ps.source       = kvs.value("source", "AUTO");
    ps.band         = kvs.value("band", "0").toInt();
    ps.rxAntenna    = kvs.value("rxant", "0").toInt();
    ps.txAntenna    = kvs.value("txant", "0").toInt();
    ps.transmitting = kvs.value("tx", "0") == "1";
    ps.inhibited    = kvs.value("inhibit", "0") == "1";
    return ps;
}

} // namespace AetherSDR
