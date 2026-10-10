#include "core/backends/hl2/Hl2Discovery.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"   // lcDiscovery

#include <QJsonObject>
#include "core/backends/hl2/MetisProtocol.h"

#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>

#include <utility>

#ifdef Q_OS_WIN
// winsock2.h pulls in windows.h, whose min/max function-like macros otherwise
// clobber std::min/std::max at their use sites (MSVC error C2589).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace AetherSDR::hl2 {

namespace {

// QUdpSocket does not enable SO_BROADCAST itself; set it on the native handle so
// the discovery datagram reaches the subnet broadcast address.
void enableBroadcast(QUdpSocket& s) noexcept
{
    const qintptr fd = s.socketDescriptor();
    if (fd < 0)
        return;
    const int on = 1;
#ifdef Q_OS_WIN
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
#else
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
}

}  // namespace

void DiscoveryNotices::bindFailed(const QString& interfaceName,
                                  const QHostAddress& address,
                                  const QString& error)
{
    const QString key = interfaceName + QLatin1Char(' ') + address.toString();
    if (m_bindFailures.contains(key))
        return;   // already reported, and it has not bound since
    m_bindFailures.insert(key);
    qCWarning(lcDiscovery).noquote()
        << QStringLiteral("HL2 discovery: cannot bind %1 %2: %3")
               .arg(interfaceName, address.toString(), error);
}

void DiscoveryNotices::bindSucceeded(const QString& interfaceName,
                                     const QHostAddress& address)
{
    m_bindFailures.remove(interfaceName + QLatin1Char(' ') + address.toString());
}

void DiscoveryNotices::endRefresh(int socketCount, int interfaceCount)
{
    m_interfaceCount = interfaceCount;
    if (socketCount > 0) {
        m_noSocketReported = false;
    } else if (!m_noSocketReported) {
        m_noSocketReported = true;
        qCWarning(lcDiscovery).noquote()
            << QStringLiteral("HL2 discovery: no usable IPv4 interface; no probe can be sent");
    }
}

void DiscoveryNotices::sendFailed(const QHostAddress& local, const QString& error)
{
    const QString key = local.toString();
    if (m_sendFailures.contains(key))
        return;   // already reported, and nothing has left this address since
    m_sendFailures.insert(key);
    qCWarning(lcDiscovery).noquote()
        << QStringLiteral("HL2 discovery: cannot send a probe from %1: %2").arg(key, error);
}

void DiscoveryNotices::sendSucceeded(const QHostAddress& local)
{
    m_sendFailures.remove(local.toString());
}

void DiscoveryNotices::sweepClosed(bool answered)
{
    if (answered) {
        m_silentSweeps = 0;
        return;
    }
    if (++m_silentSweeps != kSilentSweepsBeforeWarning)
        return;
    // Info, not a warning: an install without an HL2 is silent by design.
    qCInfo(lcDiscovery).noquote()
        << QStringLiteral("HL2 discovery: no Hermes-Lite 2 answered %1 sweeps on %2 %3")
               .arg(kSilentSweepsBeforeWarning)
               .arg(m_interfaceCount)
               .arg(m_interfaceCount == 1 ? QStringLiteral("interface")
                                          : QStringLiteral("interfaces"));
}

void DiscoveryNotices::reset()
{
    *this = DiscoveryNotices{};
}

QString Hl2Discovery::macToSerial(const std::array<std::uint8_t, 6>& mac)
{
    QStringList parts;
    parts.reserve(6);
    for (const std::uint8_t b : mac)
        parts << QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

QString Hl2Discovery::nicknameSettingsKey(const QString& serial)
{
    // Serial IS the MAC string (macToSerial), e.g. "AA:BB:CC:DD:EE:FF".
    // AppSettings writes top-level keys as XML *element names* and silently drops
    // any key that isn't ^[A-Za-z_][A-Za-z0-9_]*$ (AppSettings::save()), so the
    // colons — and a '/' separator — would never reach disk: the nickname would
    // survive the session in the in-memory map and vanish on restart. Fold every
    // illegal character to '_' so the key is writable while staying per-MAC unique.
    static const QRegularExpression kIllegal(QStringLiteral("[^A-Za-z0-9_]"));
    return QStringLiteral("Hl2Nickname_")
         + QString(serial).replace(kIllegal, QStringLiteral("_"));
}

namespace {

QString identityFamily(const QString& family)
{
    // Legacy keys never carried a family; every producer was an HL2.
    return family.isEmpty() ? QStringLiteral("hl2") : family.toLower();
}

constexpr char kIdentityFeature[] = "Identity";
constexpr char kNicknameField[] = "nickname";

} // namespace

QString Hl2Discovery::effectiveNickname(const QString& family,
                                        const QString& serial,
                                        const QString& fallback)
{
    auto& settings = AppSettings::instance();
    const QString fam = identityFamily(family);
    QString custom = settings
                         .radioFeature(fam, serial,
                                       QString::fromLatin1(kIdentityFeature))
                         .value(QLatin1String(kNicknameField))
                         .toString()
                         .trimmed();
    if (custom.isEmpty()) {
        // One-shot migration from the legacy sanitized flat key (RFC #4603
        // PR 3): adopt it into the scoped document, then drop the flat key.
        const QString legacyKey = nicknameSettingsKey(serial);
        custom = settings.value(legacyKey).toString().trimmed();
        if (!custom.isEmpty()) {
            // setNickname() removes the legacy key and commits — one write.
            setNickname(fam, serial, custom);
        }
    }
    return custom.isEmpty() ? fallback : custom;
}

void Hl2Discovery::setNickname(const QString& family, const QString& serial,
                               const QString& name)
{
    auto& settings = AppSettings::instance();
    const QString fam = identityFamily(family);
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        settings.removeRadioFeature(fam, serial,
                                    QString::fromLatin1(kIdentityFeature));
    } else {
        settings.setRadioFeature(
            fam, serial, QString::fromLatin1(kIdentityFeature), 1,
            QJsonObject{{QLatin1String(kNicknameField), trimmed}});
    }
    // A cleared name must not resurrect from the legacy key either.
    settings.remove(nicknameSettingsKey(serial));
    settings.save();
}

bool Hl2Discovery::hasCustomNickname(const QString& family, const QString& serial)
{
    return !effectiveNickname(family, serial, QString()).isEmpty();
}

bool Hl2Discovery::nicknameLivesOnRadio(const RadioInfo& info)
{
    // RadioInfo::family defaults to "flex", and legacy/default-constructed
    // entries leave it empty — both mean Flex, the only family with an
    // on-radio name store.
    return info.family.isEmpty()
        || info.family.compare(QLatin1String("flex"), Qt::CaseInsensitive) == 0;
}

Hl2Discovery::Hl2Discovery(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &Hl2Discovery::onSweepTimer);
}

Hl2Discovery::~Hl2Discovery() = default;

bool Hl2Discovery::isRunning() const noexcept
{
    return m_timer && m_timer->isActive();
}

void Hl2Discovery::start(int intervalMs)
{
    m_timer->start(intervalMs);
    sweepNow();   // don't make the operator wait a full interval for the first sweep
}

void Hl2Discovery::stop()
{
    if (m_timer)
        m_timer->stop();
    for (QUdpSocket* socket : std::as_const(m_sockets)) {
        socket->close();
        socket->deleteLater();
    }
    m_sockets.clear();
    m_seen.clear();
    m_notices.reset();
    m_probeSent = false;
    m_answered = false;
}

void Hl2Discovery::sweepNow()
{
    // Interfaces can come up after the application starts (common with a
    // directly attached HL2 link). Refreshing here also mirrors Thetis's
    // per-sweep NIC walk instead of freezing the initial adapter list.
    refreshSockets();
    m_probeSent = !m_sockets.isEmpty();
    if (!m_probeSent)
        return;
    const auto pkt = discoveryRequest();
    for (QUdpSocket* socket : std::as_const(m_sockets)) {
        const QHostAddress local = socket->localAddress();
        QHostAddress directedBroadcast;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
                if (entry.ip() == local) {
                    directedBroadcast = entry.broadcast();
                    break;
                }
            }
            if (!directedBroadcast.isNull())
                break;
        }

        bool sent = false;
        QString error;
        const auto probe = [&](const QHostAddress& to) {
            if (socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                                      static_cast<qint64>(pkt.size()),
                                      to, kMetisPort) >= 0) {
                sent = true;
            } else {
                error = socket->errorString();
            }
        };
        if (!directedBroadcast.isNull()) {
            probe(directedBroadcast);
        }
        // Keep the global broadcast as a compatibility fallback for networks
        // whose interface does not expose a usable directed broadcast.
        if (directedBroadcast != QHostAddress::Broadcast) {
            probe(QHostAddress::Broadcast);
        }
        // One of the two reaching the wire is a sent probe.
        if (sent) {
            m_notices.sendSucceeded(local);
        } else {
            m_notices.sendFailed(local, error);
        }
    }
}

void Hl2Discovery::refreshSockets()
{
    for (QUdpSocket* socket : std::as_const(m_sockets)) {
        socket->close();
        socket->deleteLater();
    }
    m_sockets.clear();
    QSet<QString> boundInterfaces;

    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }

        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress local = entry.ip();
            if (local.protocol() != QAbstractSocket::IPv4Protocol)
                continue;

            auto* socket = new QUdpSocket(this);
            if (!socket->bind(local, 0)) {
                m_notices.bindFailed(iface.humanReadableName(), local,
                                     socket->errorString());
                socket->deleteLater();
                continue;
            }
            enableBroadcast(*socket);
            connect(socket, &QUdpSocket::readyRead,
                    this, &Hl2Discovery::onReadyRead);
            m_sockets.append(socket);
            m_notices.bindSucceeded(iface.humanReadableName(), local);
            boundInterfaces.insert(iface.humanReadableName());
        }
    }
    m_notices.endRefresh(static_cast<int>(m_sockets.size()),
                         static_cast<int>(boundInterfaces.size()));
}

void Hl2Discovery::onSweepTimer()
{
    // The previous sweep's reply window ends here. A sweep with no socket to
    // send on is not counted: the no-interface warning covers it.
    if (m_probeSent)
        m_notices.sweepClosed(m_answered);
    m_answered = false;

    // Age out anything that missed too many consecutive sweeps before probing
    // again, so a radio that is unplugged disappears from the picker.
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (++it.value().missedSweeps > kMissedSweepsBeforeLost) {
            const QString serial = it.key();
            it = m_seen.erase(it);
            emit radioLost(serial);
        } else {
            ++it;
        }
    }
    sweepNow();
}

void Hl2Discovery::onReadyRead()
{
    auto* socket = qobject_cast<QUdpSocket*>(sender());
    if (!socket)
        return;

    while (socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = socket->receiveDatagram();
        const QByteArray data = dg.data();
        const auto reply = parseDiscoveryReply(
            {reinterpret_cast<const std::uint8_t*>(data.constData()),
             static_cast<std::size_t>(data.size())});
        if (!reply || !reply->isHermesLite2())
            continue;   // not an HL2 (or not a discovery reply at all)
        m_answered = true;

        RadioInfo info;
        info.family   = QStringLiteral("hl2");
        info.address  = dg.senderAddress();
        info.port     = kMetisPort;
        info.model    = QStringLiteral("Hermes-Lite 2");
        info.name     = info.model;
        info.serial   = macToSerial(reply->mac);
        // An HL2 has no on-radio name store; show the operator's custom nickname
        // (keyed by MAC/serial in AppSettings) if one is set, else the model name.
        info.nickname = effectiveNickname(QStringLiteral("hl2"), info.serial, info.model);
        info.version  = QString::number(reply->gatewareVersion);
        // A bare revision number is not self-describing in a status bar.
        info.versionLabel = QStringLiteral("Gateware");
        // A radio already streaming to another client answers with 0x03. Show it
        // as present-but-taken rather than hiding it.
        info.inUse    = reply->streaming;
        info.status   = reply->streaming ? QStringLiteral("In_Use")
                                         : QStringLiteral("Available");

        auto it = m_seen.find(info.serial);
        if (it == m_seen.end()) {
            m_seen.insert(info.serial, Seen{info, 0});
            emit radioDiscovered(info);
        } else {
            it.value().missedSweeps = 0;
            // Only re-emit when something the picker displays actually changed.
            const RadioInfo& prev = it.value().info;
            const bool changed = prev.address != info.address
                              || prev.status  != info.status
                              || prev.version != info.version
                              || prev.nickname != info.nickname;
            it.value().info = info;
            if (changed)
                emit radioUpdated(info);
        }
    }
}

}  // namespace AetherSDR::hl2
