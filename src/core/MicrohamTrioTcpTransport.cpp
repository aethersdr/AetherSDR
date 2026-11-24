#include "core/MicrohamTrioTcpTransport.h"

#include <QAbstractSocket>
#include <QTcpSocket>

namespace AetherSDR {

MicrohamTrioTcpTransport::MicrohamTrioTcpTransport(QObject* parent)
    : MicrohamTrioTransport(parent)
{
}

MicrohamTrioTcpTransport::~MicrohamTrioTcpTransport()
{
    close();
}

void MicrohamTrioTcpTransport::open(const QString& host, quint16 port)
{
    close();

    m_socket = new QTcpSocket(this);
    QTcpSocket* socket = m_socket;
    connect(socket, &QTcpSocket::connected, this, &MicrohamTrioTransport::opened);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        emit received(socket->readAll());
    });
    connect(socket, &QTcpSocket::disconnected, this, &MicrohamTrioTransport::closedByPeer);
    // A close by the peer also arrives as disconnected(); report it once.
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError error) {
                if (error != QAbstractSocket::RemoteHostClosedError) {
                    emit failed(socket->errorString());
                }
            });
    socket->connectToHost(host, port);
}

void MicrohamTrioTcpTransport::send(const QByteArray& bytes)
{
    if (m_socket != nullptr && m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write(bytes);
    }
}

void MicrohamTrioTcpTransport::close()
{
    if (m_socket == nullptr) {
        return;
    }
    QTcpSocket* socket = m_socket;
    m_socket = nullptr;
    socket->disconnect(this);
    // An orderly close when connected, as the recorded client closed.
    if (socket->state() == QAbstractSocket::ConnectedState) {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        socket->disconnectFromHost();
        if (socket->state() == QAbstractSocket::UnconnectedState) {
            socket->deleteLater();
        }
        return;
    }
    socket->abort();
    socket->deleteLater();
}

} // namespace AetherSDR
