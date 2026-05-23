#include "RadioDiscovery.h"
#include "LogManager.h"

#include <QNetworkDatagram>
#include <QDateTime>

namespace MasterSDR {

RadioDiscovery::RadioDiscovery(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &RadioDiscovery::onReadyRead);

    m_staleTimer.setInterval(STALE_TIMEOUT_MS / 2);
    connect(&m_staleTimer, &QTimer::timeout, this, &RadioDiscovery::onStaleCheck);

    m_bindRetryTimer.setInterval(BIND_RETRY_MS);
    m_bindRetryTimer.setSingleShot(true);
    connect(&m_bindRetryTimer, &QTimer::timeout, this, &RadioDiscovery::onBindRetry);

    // Periodic re-bind: handles the case where bind() succeeds but the socket
    // is stale (e.g. no network interface at launch, macOS consent silently
    // dropping packets).  Stops once the first discovery packet arrives.
    m_rebindTimer.setInterval(REBIND_INTERVAL_MS);
    connect(&m_rebindTimer, &QTimer::timeout, this, [this]() {
        qCDebug(lcDiscovery) << "RadioDiscovery: no packets yet, re-binding socket";
        startListening();
    });
}

RadioDiscovery::~RadioDiscovery()
{
    stopListening();
}

void RadioDiscovery::startListening()
{
    // Close any previous socket state before (re-)binding
    m_socket.close();

    if (!m_socket.bind(QHostAddress::AnyIPv4, DISCOVERY_PORT,
                       QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qCWarning(lcDiscovery) << "RadioDiscovery: bind failed on UDP port" << DISCOVERY_PORT
                               << "–" << m_socket.errorString();

        if (m_bindRetryCount < MAX_BIND_RETRIES) {
            ++m_bindRetryCount;
            qCDebug(lcDiscovery) << "RadioDiscovery: scheduling retry"
                                 << m_bindRetryCount << "/" << MAX_BIND_RETRIES;
            m_bindRetryTimer.start();
        } else {
            qCWarning(lcDiscovery) << "RadioDiscovery: giving up after"
                                   << MAX_BIND_RETRIES << "retries";
        }
        return;
    }

    if (m_bindRetryCount > 0) {
        qCDebug(lcDiscovery) << "RadioDiscovery: bind succeeded after"
                             << m_bindRetryCount << "retries";
    }
    m_bindRetryCount = 0;
    m_staleTimer.start();

    // Keep re-binding periodically until we actually receive a discovery packet.
    // Covers: no network at launch, macOS consent blocking packets, interface changes.
    if (!m_receivedAny) {
        m_rebindTimer.start();
    }

    qCDebug(lcDiscovery) << "RadioDiscovery: listening on UDP" << DISCOVERY_PORT;
}

void RadioDiscovery::stopListening()
{
    m_bindRetryTimer.stop();
    m_rebindTimer.stop();
    m_bindRetryCount = 0;
    m_receivedAny = false;
    m_staleTimer.stop();
    m_socket.close();
}

void RadioDiscovery::onBindRetry()
{
    startListening();
}

// SmartSDR discovery packets are ASCII key=value pairs separated by spaces.
// Example:
//   name=my-flex model=FLEX-6600 serial=1234ABCD version=3.3.28.0
//   ip=192.168.1.50 port=4992 status=Available max_licensed_version=3
RadioInfo RadioDiscovery::parseDiscoveryPacket(const QByteArray& data) const
{
    RadioInfo info;
    const QString text = QString::fromUtf8(data).trimmed();

    for (const QString& token : text.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq < 0) continue;

        const QString key   = token.left(eq).toLower();
        const QString value = token.mid(eq + 1);

        if      (key == "name")    info.name    = value;
        else if (key == "model")   info.model   = value;
        else if (key == "serial")  info.serial  = value;
        else if (key == "version") info.version = value;
        else if (key == "ip")      info.address = QHostAddress(value);
        else if (key == "port")    info.port    = value.toUShort();
        else if (key == "status")  info.status  = value;
        else if (key == "nickname") info.nickname = value;
        else if (key == "callsign") info.callsign = value;
        else if (key == "inuse")       info.inUse           = (value == "1");
        else if (key == "mf_enable")   info.multiFlexEnabled = (value != "0");
        else if (key == "max_licensed_version") info.maxLicensedVersion = value.toInt();
        else if (key == "is_system_model") info.isSystemModel = (value == "1");
        else if (key == "turf_region")     info.turfRegion    = value;
        else if (key == "gui_client_stations") {
            QString cleaned = value;
            cleaned.replace('\x7f', ' ');
            info.guiClientStations = cleaned.split(',', Qt::SkipEmptyParts);
        }
        else if (key == "gui_client_handles")
            info.guiClientHandles = value.split(',', Qt::SkipEmptyParts);
        else if (key == "gui_client_programs") {
            QString cleaned = value;
            cleaned.replace('\x7f', ' ');
            info.guiClientPrograms = cleaned.split(',', Qt::SkipEmptyParts);
        }
        else if (key == "gui_client_ips")
            info.guiClientIps = value.split(',', Qt::SkipEmptyParts);
        else if (key == "gui_client_hosts") {
            QString cleaned = value;
            cleaned.replace('\x7f', ' ');
            info.guiClientHosts = cleaned.split(',', Qt::SkipEmptyParts);
        }
    }
    return info;
}

void RadioDiscovery::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket.receiveDatagram();
        if (datagram.isNull()) continue;

        // First packet received — stop the periodic re-bind, socket is healthy
        if (!m_receivedAny) {
            m_receivedAny = true;
            m_rebindTimer.stop();
            qCDebug(lcDiscovery) << "RadioDiscovery: first packet received, re-bind timer stopped";
        }

        RadioInfo info = parseDiscoveryPacket(datagram.data());

        // Fall back to sender address if the packet didn't include an IP field
        if (info.address.isNull())
            info.address = datagram.senderAddress();

        if (info.serial.isEmpty()) {
            qCWarning(lcDiscovery) << "RadioDiscovery: received packet without serial, ignoring";
            continue;
        }

        m_lastSeen[info.serial] = QDateTime::currentMSecsSinceEpoch();
        upsertRadio(info);
    }
}

void RadioDiscovery::upsertRadio(const RadioInfo& info)
{
    for (int i = 0; i < m_radios.size(); ++i) {
        if (m_radios[i].serial == info.serial) {
            m_radios[i] = info;
            emit radioUpdated(info);
            return;
        }
    }
    m_radios.append(info);
    qCDebug(lcDiscovery) << "RadioDiscovery: found" << info.displayName();
    emit radioDiscovered(info);
}

void RadioDiscovery::onStaleCheck()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList lost;

    for (auto it = m_lastSeen.cbegin(); it != m_lastSeen.cend(); ++it) {
        if (now - it.value() > STALE_TIMEOUT_MS)
            lost.append(it.key());
    }

    for (const QString& serial : lost) {
        m_lastSeen.remove(serial);
        m_radios.removeIf([&](const RadioInfo& r){ return r.serial == serial; });
        qCDebug(lcDiscovery) << "RadioDiscovery: lost radio" << serial;
        emit radioLost(serial);
    }
}

} // namespace MasterSDR
