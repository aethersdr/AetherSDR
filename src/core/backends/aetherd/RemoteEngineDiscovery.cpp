#include "RemoteEngineDiscovery.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QUdpSocket>
#include <QUrl>

namespace AetherSDR::aetherd {

namespace {
constexpr quint16 kDiscoveryPort = 4996;   // DiscoveryResponder::kPort, fixed by ruling
constexpr qsizetype kMaxReplyBytes = 1024;
const QByteArray kProbe = QByteArrayLiteral("AETHERD-DISCOVER 1");
} // namespace

RemoteEngineDiscovery::RemoteEngineDiscovery(QObject* parent) : QObject(parent) {}

RemoteEngineDiscovery::~RemoteEngineDiscovery()
{
    stop();
}

bool RemoteEngineDiscovery::start()
{
    if (m_socket) { return true; }
    auto socket = std::make_unique<QUdpSocket>();
    if (!socket->bind(QHostAddress::AnyIPv4, 0)) { return false; }
    connect(socket.get(), &QUdpSocket::readyRead, this, [this] { readReplies(); });
    m_socket = std::move(socket);
    return true;
}

void RemoteEngineDiscovery::stop()
{
    if (!m_socket) { return; }
    m_socket->disconnect(this);
    m_socket->close();
    m_socket.reset();
}

void RemoteEngineDiscovery::setProbeTargetForTest(const QHostAddress& address, quint16 port)
{
    m_testTarget = std::make_pair(address, port);
}

void RemoteEngineDiscovery::probe()
{
    if (!m_socket && !start()) { return; }
    if (m_testTarget) {
        m_socket->writeDatagram(kProbe, m_testTarget->first, m_testTarget->second);
        return;
    }
    QSet<QString> sent;
    const auto send = [&](const QHostAddress& target) {
        if (target.isNull() || sent.contains(target.toString())) { return; }
        sent.insert(target.toString());
        m_socket->writeDatagram(kProbe, target, kDiscoveryPort);
    };
    send(QHostAddress::Broadcast);
    // Not "interface": that is a macro once windows.h is in the translation unit.
    for (const QNetworkInterface& candidate : QNetworkInterface::allInterfaces()) {
        const auto flags = candidate.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::CanBroadcast)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : candidate.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) { send(entry.broadcast()); }
        }
    }
}

std::optional<RemoteEngineDiscovery::Advert> RemoteEngineDiscovery::parseReply(
    const QByteArray& datagram, const QHostAddress& sender)
{
    if (datagram.isEmpty() || datagram.size() > kMaxReplyBytes) { return std::nullopt; }
    const QJsonObject reply = QJsonDocument::fromJson(datagram).object();
    if (reply.value(QStringLiteral("aetherd")).toInt() != 1) { return std::nullopt; }
    static const QRegularExpression hex(QStringLiteral("^[a-f0-9]{64}$"));
    const QString fingerprint = reply.value(QStringLiteral("certificateSha256")).toString();
    if (!hex.match(fingerprint).hasMatch()) { return std::nullopt; }
    const QUrl url(reply.value(QStringLiteral("wss")).toString(), QUrl::StrictMode);
    QHostAddress host;
    if (!url.isValid() || url.scheme() != QLatin1String("wss") || !host.setAddress(url.host())
        || url.port() <= 0 || !url.path().isEmpty()) {
        return std::nullopt;
    }
    // The advertised listener must be the machine that answered: a reply
    // never redirects the desktop to a third address.
    bool mapped = false;
    const quint32 v4 = sender.toIPv4Address(&mapped);
    const QHostAddress from = mapped ? QHostAddress(v4) : sender;
    if (!host.isEqual(from, QHostAddress::ConvertV4MappedToIPv4)) { return std::nullopt; }
    const QJsonObject radio = reply.value(QStringLiteral("radio")).toObject();
    Advert advert;
    advert.name = reply.value(QStringLiteral("name")).toString().left(64);
    advert.model = radio.value(QStringLiteral("model")).toString().left(64);
    advert.family = radio.value(QStringLiteral("family")).toString().left(64);
    advert.radioConnected = radio.value(QStringLiteral("connected")).toBool();
    advert.wssUrl = url.toString();
    advert.certificateSha256 = fingerprint;
    advert.address = host;
    advert.port = quint16(url.port());
    return advert;
}

void RemoteEngineDiscovery::readReplies()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram(kMaxReplyBytes + 1);
        if (const auto advert = parseReply(datagram.data(), datagram.senderAddress())) {
            emit engineFound(*advert);
        }
    }
}

} // namespace AetherSDR::aetherd
