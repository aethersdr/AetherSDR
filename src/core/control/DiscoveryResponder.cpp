#include "DiscoveryResponder.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QUdpSocket>

namespace AetherSDR::control {

namespace {

constexpr QLatin1StringView kProbe{"AETHERD-DISCOVER 1"};

QString bounded(const QString& text)
{
    return text.left(DiscoveryResponder::kMaxFieldChars);
}

// The interface name that owns this local (destination) address, or empty.
QString interfaceHolding(const QHostAddress& address)
{
    // Not "interface": that is a macro once windows.h is in the translation unit.
    for (const QNetworkInterface& candidate : QNetworkInterface::allInterfaces()) {
        for (const QNetworkAddressEntry& entry : candidate.addressEntries()) {
            if (entry.ip().isEqual(address, QHostAddress::ConvertV4MappedToIPv4)) {
                return candidate.name();
            }
        }
    }
    return {};
}

} // namespace

DiscoveryResponder::DiscoveryResponder(Config config, QObject* parent)
    : QObject(parent), m_config(std::move(config))
{
    if (!m_config.clock) {
        auto timer = std::make_shared<QElapsedTimer>();
        timer->start();
        m_config.clock = [timer] { return timer->elapsed(); };
    }
}

DiscoveryResponder::~DiscoveryResponder()
{
    stop();
}

bool DiscoveryResponder::start(QString* error)
{
    if (m_socket) { return true; }
    auto socket = std::make_unique<QUdpSocket>();
    // A wildcard bind is what receives LAN broadcasts; the reply is unicast.
    if (!socket->bind(m_config.bindAddress, m_config.port)) {
        if (error) { *error = socket->errorString(); }
        return false;
    }
    connect(socket.get(), &QUdpSocket::readyRead, this, [this] { readPending(); });
    m_socket = std::move(socket);
    return true;
}

void DiscoveryResponder::stop()
{
    if (!m_socket) { return; }
    m_socket->disconnect(this);
    m_socket->close();
    m_socket.reset();
}

bool DiscoveryResponder::isListening() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

quint16 DiscoveryResponder::boundPort() const
{
    return m_socket ? m_socket->localPort() : 0;
}

bool DiscoveryResponder::isProbe(const QByteArray& datagram)
{
    return datagram == QByteArrayView(kProbe.data(), kProbe.size());
}

bool DiscoveryResponder::isWireGuardInterfaceName(const QString& name)
{
    return name.startsWith(QLatin1String("tailscale")) || name.startsWith(QLatin1String("wg"));
}

bool DiscoveryResponder::acceptsSource(const QHostAddress& source, const QString& arrivalInterface,
                                       bool acceptLoopback)
{
    if (isWireGuardInterfaceName(arrivalInterface)) { return false; }
    QHostAddress address = source;
    bool mapped = false;
    const quint32 v4 = source.toIPv4Address(&mapped);
    if (mapped) { address = QHostAddress(v4); }
    if (address.isNull()) { return false; }
    if (address.isLoopback()) { return acceptLoopback; }
    // Shared (carrier-grade NAT) space, which Tailscale numbers its IPv4 from,
    // and Tailscale's IPv6 prefix, which is inside the ULA range accepted below.
    if (address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)
        || address.isInSubnet(QHostAddress(QStringLiteral("fd7a:115c:a1e0::")), 48)) {
        return false;
    }
    return address.isPrivateUse() || address.isLinkLocal();
}

QByteArray DiscoveryResponder::buildReply(const QString& name, const Radio& radio,
                                          const QString& wssUrl, const QString& certificateSha256)
{
    const QJsonObject reply{
        {QStringLiteral("aetherd"), 1},
        {QStringLiteral("name"), bounded(name)},
        {QStringLiteral("radio"), QJsonObject{
             {QStringLiteral("model"), bounded(radio.model)},
             {QStringLiteral("family"), bounded(radio.family)},
             {QStringLiteral("connected"), radio.connected}}},
        {QStringLiteral("wss"), wssUrl},
        {QStringLiteral("certificateSha256"), certificateSha256}};
    QByteArray bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    if (bytes.size() > kMaxReplyBytes) { return {}; }
    return bytes;
}

bool DiscoveryResponder::takeReplySlot(qint64 nowMs)
{
    while (!m_recentReplies.empty() && nowMs - m_recentReplies.front() >= 1000) {
        m_recentReplies.pop_front();
    }
    if (std::ssize(m_recentReplies) >= kMaxRepliesPerSecond) { return false; }
    m_recentReplies.push_back(nowMs);
    return true;
}

void DiscoveryResponder::readPending()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        // Larger than any probe, so an oversized datagram reads as "not a probe".
        const QNetworkDatagram datagram = m_socket->receiveDatagram(64);
        if (!datagram.isValid() || !isProbe(datagram.data())) { continue; }
        QString arrival;
        if (datagram.interfaceIndex() != 0) {
            arrival = QNetworkInterface::interfaceNameFromIndex(int(datagram.interfaceIndex()));
        }
        if (arrival.isEmpty() && !datagram.destinationAddress().isNull()) {
            arrival = interfaceHolding(datagram.destinationAddress());
        }
        // Without the arrival interface, a tailnet probe cannot be told apart.
        if (arrival.isEmpty()) { continue; }
        if (!acceptsSource(datagram.senderAddress(), arrival, m_config.acceptLoopback)) { continue; }
        if (!takeReplySlot(m_config.clock())) { continue; }
        const QByteArray reply = buildReply(
            m_config.name, m_config.radio ? m_config.radio() : Radio{},
            m_config.wssUrl, m_config.certificateSha256);
        if (reply.isEmpty()) { continue; }
        m_socket->writeDatagram(reply, datagram.senderAddress(), quint16(datagram.senderPort()));
    }
}

} // namespace AetherSDR::control
