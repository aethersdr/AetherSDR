#include "core/backends/anan/AnanDiscovery.h"

#include "core/AppSettings.h"
#include "core/LogManager.h"   // lcDiscovery
#include "core/backends/anan/P2Protocol.h"

#include <QJsonObject>
#include <QNetworkDatagram>
#include <QStringList>
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

namespace AetherSDR::anan {

namespace {

// QUdpSocket does not enable SO_BROADCAST itself; set it on the native handle
// so the discovery datagram reaches the subnet broadcast address. Duplicated
// from Hl2Discovery.cpp's helper of the same name rather than factored into a
// shared header -- matches this codebase's existing per-file convention
// (MetisClient.cpp carries its own copy too).
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

// Every address entry on an interface that is actually carrying traffic.
// NOT testable, which is why it is the only part of the sweep that lives here:
// QNetworkInterface has no public constructor to synthesise one with. The flag
// test is the same one NetworkPathResolver::enumerateIpv4Candidates() applies,
// minus its IsLoopBack check -- broadcastTargets() drops loopback by address,
// which also catches a loopback address on an interface not flagged as one.
QList<QNetworkAddressEntry> liveAddressEntries()
{
    QList<QNetworkAddressEntry> entries;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning))
            continue;
        entries.append(iface.addressEntries());
    }
    return entries;
}

constexpr char kIdentityFeature[] = "Identity";
constexpr char kNicknameField[] = "nickname";

// How a probe target reads in the log. The global broadcast is named, not
// printed: the log file keeps only an address's last octet, and
// 255.255.255.255 would read like any /24's directed broadcast.
QString targetName(const QHostAddress& target)
{
    return target == QHostAddress(QHostAddress::Broadcast)
               ? QStringLiteral("the global broadcast")
               : target.toString();
}

}  // namespace

void AnanDiscoveryLog::bindFailed(const QString& error)
{
    if (m_bindFailureReported) {
        return;   // already reported, and no bind has succeeded since
    }
    m_bindFailureReported = true;
    qCWarning(lcDiscovery).noquote()
        << QStringLiteral("ANAN discovery: cannot bind a UDP socket: %1").arg(error);
}

void AnanDiscoveryLog::bindSucceeded()
{
    m_bindFailureReported = false;
}

void AnanDiscoveryLog::targetsListed(const QList<QHostAddress>& targets)
{
    // broadcastTargets() puts the global broadcast first; anything after it
    // is a subnet's directed broadcast.
    if (targets.size() > 1) {
        m_noSubnetReported = false;
    } else if (!m_noSubnetReported) {
        m_noSubnetReported = true;
        qCWarning(lcDiscovery).noquote()
            << QStringLiteral("ANAN discovery: no IPv4 interface has a broadcast address; probing the global broadcast only");
    }
}

void AnanDiscoveryLog::sendFailed(const QHostAddress& target, const QString& error)
{
    const QString key = target.toString();
    if (m_sendFailures.contains(key)) {
        return;   // already reported, and no probe has left for it since
    }
    m_sendFailures.insert(key);
    qCWarning(lcDiscovery).noquote()
        << QStringLiteral("ANAN discovery: cannot send a probe to %1: %2")
               .arg(targetName(target), error);
}

void AnanDiscoveryLog::sendSucceeded(const QHostAddress& target)
{
    const QString key = target.toString();
    m_sendFailures.remove(key);
    m_sentTargets.insert(key);
}

void AnanDiscoveryLog::replyListed()
{
    m_answered = true;
}

void AnanDiscoveryLog::sweepClosed()
{
    // Both marks belong to the sweep being closed, answered or not.
    const int sentTo = static_cast<int>(std::exchange(m_sentTargets, {}).size());
    if (std::exchange(m_answered, false)) {
        m_silentSweeps = 0;
        return;
    }
    if (sentTo == 0) {
        return;   // nothing was asked, so nothing went unanswered
    }
    if (++m_silentSweeps != kSilentSweepsBeforeNotice) {
        return;
    }
    // Info, not a warning: every install looks for an ANAN-G2, and most have none.
    qCInfo(lcDiscovery).noquote()
        << QStringLiteral("ANAN discovery: no ANAN-G2 answered %1 sweeps to %2 %3")
               .arg(kSilentSweepsBeforeNotice)
               .arg(sentTo)
               .arg(sentTo == 1 ? QStringLiteral("address")
                                : QStringLiteral("addresses"));
}

void AnanDiscoveryLog::reset()
{
    *this = AnanDiscoveryLog{};
}

QList<QHostAddress> AnanDiscovery::broadcastTargets(
    const QList<QNetworkAddressEntry>& entries)
{
    // The global broadcast goes first and unconditionally -- see this function's
    // declaration comment for why it is kept rather than replaced.
    QList<QHostAddress> targets{QHostAddress(QHostAddress::Broadcast)};
    for (const QNetworkAddressEntry& entry : entries) {
        const QHostAddress broadcast = entry.broadcast();
        // Protocol 2 discovery is IPv4-only; absent broadcast addresses are
        // not valid datagram destinations.
        if (broadcast.isNull() || broadcast.protocol() != QAbstractSocket::IPv4Protocol) {
            continue;
        }
        // /31 and /32 links have no broadcast domain. Qt can still synthesize
        // ip | ~netmask for them, including this host's own address for a /32.
        if (entry.prefixLength() >= 31) {
            continue;
        }
        // The loopback subnet broadcasts to this host only; the radio is not
        // there, and p2app on the same machine would be a different setup.
        if (entry.ip().isLoopback())
            continue;
        if (!targets.contains(broadcast))
            targets.append(broadcast);
    }
    return targets;
}

QString AnanDiscovery::macToSerial(const std::array<std::uint8_t, 6>& mac)
{
    QStringList parts;
    parts.reserve(6);
    for (const std::uint8_t b : mac)
        parts << QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

QString AnanDiscovery::effectiveNickname(const QString& family, const QString& serial,
                                         const QString& fallback)
{
    auto& settings = AppSettings::instance();
    const QString custom = settings
                                .radioFeature(family, serial,
                                              QString::fromLatin1(kIdentityFeature))
                                .value(QLatin1String(kNicknameField))
                                .toString()
                                .trimmed();
    return custom.isEmpty() ? fallback : custom;
}

void AnanDiscovery::setNickname(const QString& family, const QString& serial,
                                const QString& name)
{
    auto& settings = AppSettings::instance();
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        settings.removeRadioFeature(family, serial, QString::fromLatin1(kIdentityFeature));
    } else {
        settings.setRadioFeature(
            family, serial, QString::fromLatin1(kIdentityFeature), 1,
            QJsonObject{{QLatin1String(kNicknameField), trimmed}});
    }
    settings.save();
}

AnanDiscovery::AnanDiscovery(QObject* parent) : QObject(parent)
{
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &AnanDiscovery::onSweepTimer);
}

AnanDiscovery::~AnanDiscovery() = default;

bool AnanDiscovery::isRunning() const noexcept
{
    return m_timer && m_timer->isActive();
}

void AnanDiscovery::start(int intervalMs)
{
    // The timer starts even if sweepNow()'s bind fails: every sweep retries
    // it, so discovery recovers without a restart.
    m_timer->start(intervalMs);
    sweepNow();   // don't make the operator wait a full interval for the first sweep
}

void AnanDiscovery::stop()
{
    if (m_timer)
        m_timer->stop();
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_seen.clear();
    m_log.reset();
}

bool AnanDiscovery::openSocket()
{
    auto* socket = new QUdpSocket(this);
    if (!socket->bind(QHostAddress::AnyIPv4, 0)) {
        m_log.bindFailed(socket->errorString());
        socket->deleteLater();
        return false;
    }
    m_log.bindSucceeded();
    enableBroadcast(*socket);
    connect(socket, &QUdpSocket::readyRead, this, &AnanDiscovery::onReadyRead);
    m_socket = socket;
    return true;
}

void AnanDiscovery::sweepNow()
{
    // start() starts the timer before its first sweep, and stop() stops it,
    // so this is also the stopped case.
    if (!isRunning()) {
        return;
    }
    // One socket for the session: a wildcard bind does not depend on which
    // interfaces are up, so it is made once, when there is none.
    if (!m_socket && !openSocket()) {
        return;
    }
    const auto pkt = buildDiscovery();
    // The kernel routes each directed broadcast from this wildcard socket.
    // This reaches a second NIC on a distinct subnet; two NICs on the same
    // subnet still follow the preferred route rather than each being swept.
    // Replies return to this socket and are de-duplicated by MAC in onReadyRead().
    const QList<QHostAddress> targets = broadcastTargets(liveAddressEntries());
    m_log.targetsListed(targets);
    for (const QHostAddress& target : targets) {
        if (m_socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                                    static_cast<qint64>(pkt.size()),
                                    target, kRadioPort) >= 0) {
            m_log.sendSucceeded(target);
        } else {
            m_log.sendFailed(target, m_socket->errorString());
        }
    }
}

void AnanDiscovery::onSweepTimer()
{
    // The previous sweep's reply window ends here.
    m_log.sweepClosed();

    // Age out anything that missed too many consecutive sweeps before
    // probing again, so a radio that is unplugged disappears from the picker.
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

void AnanDiscovery::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const QByteArray data = dg.data();
        const auto reply = parseDiscoveryReply(
            {reinterpret_cast<const std::uint8_t*>(data.constData()),
             static_cast<std::size_t>(data.size())});
        if (!reply || !reply->isSaturn())
            continue;   // not an ANAN-G2 (or not a discovery reply at all)
        m_log.replyListed();

        RadioInfo info;
        info.family   = QStringLiteral("anan");
        info.address  = dg.senderAddress();
        info.port     = kRadioPort;
        info.model    = QStringLiteral("ANAN-G2");
        info.name     = info.model;
        info.serial   = macToSerial(reply->mac);
        info.nickname = effectiveNickname(QStringLiteral("anan"), info.serial, info.model);
        info.version  = QString::number(reply->firmwareVer);
        // A bare integer is not self-describing in a status bar -- and this
        // one especially: it is a gateware bitstream number, not a software
        // version (discrepancy #1, see P2Protocol.h's DiscoveryReply).
        info.versionLabel = QStringLiteral("Gateware");
        // A radio already streaming to another client answers with 0x03.
        // Show it as present-but-taken rather than hiding it.
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
                              || prev.version != info.version;
            it.value().info = info;
            if (changed)
                emit radioUpdated(info);
        }
    }
}

}  // namespace AetherSDR::anan
