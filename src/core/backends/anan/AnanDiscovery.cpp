#include "core/backends/anan/AnanDiscovery.h"

#include "core/AppSettings.h"
#include "core/backends/anan/P2Protocol.h"

#include <QJsonObject>
#include <QNetworkDatagram>
#include <QStringList>
#include <QTimer>
#include <QUdpSocket>

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

constexpr char kIdentityFeature[] = "Identity";
constexpr char kNicknameField[] = "nickname";

}  // namespace

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
    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
            m_socket->deleteLater();
            m_socket = nullptr;
            return;   // no socket: stay silent rather than half-running
        }
        enableBroadcast(*m_socket);
        connect(m_socket, &QUdpSocket::readyRead, this, &AnanDiscovery::onReadyRead);
    }
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
}

void AnanDiscovery::sweepNow()
{
    if (!m_socket)
        return;
    const auto pkt = buildDiscovery();
    m_socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                            static_cast<qint64>(pkt.size()),
                            QHostAddress::Broadcast, kRadioPort);
}

void AnanDiscovery::onSweepTimer()
{
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
