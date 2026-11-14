#include "core/backends/ReconnectBusyProbe.h"

#include <QTimer>
#include <QUdpSocket>

#include <utility>

namespace AetherSDR {

ReconnectBusyProbe::ReconnectBusyProbe(QObject* parent)
    : QObject(parent)
{
}

void ReconnectBusyProbe::start(const QHostAddress& host, quint16 port, const QByteArray& request,
                               Parse parse, Done done)
{
    cancel();
    m_done = std::move(done);
    const quint64 generation = m_generation;
    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        finish(generation, std::nullopt);   // no probe possible: never blocks
        return;
    }
    const QHostAddress radio(host.toIPv4Address());
    connect(m_socket, &QUdpSocket::readyRead, this, [this, generation, radio, parse] {
        while (m_socket && generation == m_generation && m_socket->hasPendingDatagrams()) {
            QByteArray datagram(static_cast<qsizetype>(m_socket->pendingDatagramSize()), Qt::Uninitialized);
            QHostAddress sender;
            m_socket->readDatagram(datagram.data(), datagram.size(), &sender);
            if (QHostAddress(sender.toIPv4Address()) != radio)
                continue;
            const std::optional<bool> streaming = parse(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(datagram.constData()),
                static_cast<std::size_t>(datagram.size())));
            if (streaming) {
                finish(generation, streaming);
                return;
            }
        }
    });
    m_socket->writeDatagram(request, radio, port);
    QTimer::singleShot(kTimeoutMs, this, [this, generation] { finish(generation, std::nullopt); });
}

void ReconnectBusyProbe::cancel()
{
    ++m_generation;
    m_done = nullptr;
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void ReconnectBusyProbe::armForTest(Done done)
{
    cancel();
    m_done = std::move(done);
}

void ReconnectBusyProbe::finish(quint64 generation, std::optional<bool> streaming)
{
    if (generation != m_generation || !m_done)
        return;   // superseded, cancelled, or already answered
    Done done = std::move(m_done);
    m_done = nullptr;
    ++m_generation;
    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    done(streaming);
}

} // namespace AetherSDR
