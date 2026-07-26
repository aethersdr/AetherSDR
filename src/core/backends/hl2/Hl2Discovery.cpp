#include "core/backends/hl2/Hl2Discovery.h"

#include "core/AppSettings.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QNetworkDatagram>
#include <QRegularExpression>
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

QString macToSerial(const std::array<std::uint8_t, 6>& mac)
{
    QStringList parts;
    parts.reserve(6);
    for (const std::uint8_t b : mac)
        parts << QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0')).toUpper();
    return parts.join(QLatin1Char(':'));
}

}  // namespace

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

QString Hl2Discovery::effectiveNickname(const QString& serial, const QString& fallback)
{
    const QString custom =
        AppSettings::instance().value(nicknameSettingsKey(serial)).toString().trimmed();
    return custom.isEmpty() ? fallback : custom;
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
    if (!m_socket) {
        m_socket = new QUdpSocket(this);
        if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
            m_socket->deleteLater();
            m_socket = nullptr;
            return;   // no socket: stay silent rather than half-running
        }
        enableBroadcast(*m_socket);
        connect(m_socket, &QUdpSocket::readyRead, this, &Hl2Discovery::onReadyRead);
    }
    m_timer->start(intervalMs);
    sweepNow();   // don't make the operator wait a full interval for the first sweep
}

void Hl2Discovery::stop()
{
    if (m_timer)
        m_timer->stop();
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_seen.clear();
}

void Hl2Discovery::sweepNow()
{
    if (!m_socket)
        return;
    const auto pkt = discoveryRequest();
    m_socket->writeDatagram(reinterpret_cast<const char*>(pkt.data()),
                            static_cast<qint64>(pkt.size()),
                            QHostAddress::Broadcast, kMetisPort);
}

void Hl2Discovery::onSweepTimer()
{
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
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket->receiveDatagram();
        const QByteArray data = dg.data();
        const auto reply = parseDiscoveryReply(
            {reinterpret_cast<const std::uint8_t*>(data.constData()),
             static_cast<std::size_t>(data.size())});
        if (!reply || !reply->isHermesLite2())
            continue;   // not an HL2 (or not a discovery reply at all)

        RadioInfo info;
        info.family   = QStringLiteral("hl2");
        info.address  = dg.senderAddress();
        info.port     = kMetisPort;
        info.model    = QStringLiteral("Hermes-Lite 2");
        info.name     = info.model;
        info.serial   = macToSerial(reply->mac);
        // An HL2 has no on-radio name store; show the operator's custom nickname
        // (keyed by MAC/serial in AppSettings) if one is set, else the model name.
        info.nickname = effectiveNickname(info.serial, info.model);
        info.version  = QString::number(reply->gatewareVersion);
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
