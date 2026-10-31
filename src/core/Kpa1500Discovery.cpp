#include "Kpa1500Discovery.h"
#include "LogManager.h"

#include <QAbstractSocket>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>

#include <algorithm>

namespace AetherSDR {

namespace {

// Upper bound on one sweep, across every interface.
constexpr int kMaxTargets = 4096;
constexpr int kWholeSubnetMinPrefix = 22;

}  // namespace

Kpa1500Discovery::Kpa1500Discovery(QObject* parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &Kpa1500Discovery::onReadyRead);
    m_sendTimer.setInterval(kSendIntervalMs);
    connect(&m_sendTimer, &QTimer::timeout, this, &Kpa1500Discovery::sendNextBatch);
    m_graceTimer.setSingleShot(true);
    m_graceTimer.setInterval(kReplyGraceMs);
    connect(&m_graceTimer, &QTimer::timeout, this, &Kpa1500Discovery::finish);
}

QList<QHostAddress> Kpa1500Discovery::sweepTargets(const QHostAddress& local, int prefixLength)
{
    QList<QHostAddress> out;
    if (local.protocol() != QAbstractSocket::IPv4Protocol || local.isLoopback()
        || prefixLength < 0 || prefixLength > 30) {
        return out;
    }
    const int prefix = prefixLength >= kWholeSubnetMinPrefix ? prefixLength : 24;
    const quint32 self = local.toIPv4Address();
    const quint32 mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
    const quint32 network = self & mask;
    const quint32 broadcast = network | ~mask;
    for (quint32 host = network + 1; host < broadcast; ++host) {
        if (host != self) {
            out.append(QHostAddress(host));
        }
    }
    return out;
}

void Kpa1500Discovery::start(quint16 port)
{
    QList<QHostAddress> targets;
    QSet<quint32> seen;
    auto add = [&targets, &seen](const QHostAddress& a) {
        if (targets.size() >= kMaxTargets) {
            return;
        }
        const quint32 v = a.toIPv4Address();
        if (v != 0 && !seen.contains(v)) {
            seen.insert(v);
            targets.append(a);
        }
    };
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                continue;
            }
            // The subnet broadcast first: if the amp answers it, the sweep
            // is just confirmation.
            if (!entry.broadcast().isNull()) {
                add(entry.broadcast());
            }
            for (const QHostAddress& host : sweepTargets(entry.ip(), entry.prefixLength())) {
                add(host);
            }
        }
    }
    qCInfo(lcTuner) << "Kpa1500Discovery: probing" << targets.size()
                    << "addresses on UDP port" << port;
    startWithTargets(targets, port);
}

void Kpa1500Discovery::startWithTargets(const QList<QHostAddress>& targets, quint16 port)
{
    cancel();
    m_targets = targets;
    m_port = port;
    m_next = 0;
    m_found.clear();
    if (m_socket.state() != QAbstractSocket::BoundState
        && !m_socket.bind(QHostAddress::AnyIPv4, 0)) {
        qCWarning(lcTuner) << "Kpa1500Discovery: cannot open a UDP socket:" << m_socket.errorString();
        emit finished(0);
        return;
    }
    m_running = true;
    sendNextBatch();
    m_sendTimer.start();
}

void Kpa1500Discovery::cancel()
{
    m_sendTimer.stop();
    m_graceTimer.stop();
    m_running = false;
}

QList<Kpa1500Discovery::Found> Kpa1500Discovery::results() const
{
    QList<Found> list = m_found.values();
    std::sort(list.begin(), list.end(), [](const Found& a, const Found& b) {
        return a.address.toIPv4Address() < b.address.toIPv4Address();
    });
    return list;
}

void Kpa1500Discovery::sendNextBatch()
{
    const QByteArray probe = Kpa1500::buildQuery(QStringLiteral("SN"));
    for (int i = 0; i < kSendBatch && m_next < m_targets.size(); ++i, ++m_next) {
        m_socket.writeDatagram(probe, m_targets.at(m_next), m_port);
    }
    if (m_next >= m_targets.size()) {
        m_sendTimer.stop();
        m_graceTimer.start();
    }
}

void Kpa1500Discovery::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram d = m_socket.receiveDatagram(1024);
        if (!m_running || !d.isValid() || d.senderPort() != m_port) {
            continue;
        }
        bool ok = false;
        const quint32 ip = d.senderAddress().toIPv4Address(&ok);
        if (!ok) {
            continue;
        }
        Kpa1500::MessageParser parser;
        Kpa1500::Status reply;
        parser.setMessageCallback([&reply](const Kpa1500::Message& m) {
            Kpa1500::applyMessage(m, reply);
        });
        parser.feed(d.data());

        if (reply.serialNumber && !m_found.contains(ip)) {
            Found f{QHostAddress(ip), *reply.serialNumber, {}};
            m_found.insert(ip, f);
            qCInfo(lcTuner) << "Kpa1500Discovery: KPA1500 S/N" << f.serialNumber
                            << "at" << f.address.toString();
            m_socket.writeDatagram(Kpa1500::buildQuery(QStringLiteral("RV")), f.address, m_port);
            emit found(f);
        }
        if (reply.firmware && m_found.contains(ip)) {
            m_found[ip].firmware = *reply.firmware;
        }
    }
}

void Kpa1500Discovery::finish()
{
    m_running = false;
    emit finished(static_cast<int>(m_found.size()));
}

}  // namespace AetherSDR
