#pragma once

#include "ByteRelay.h"

#include <QAbstractSocket>
#include <QTcpSocket>

namespace AetherSDR {

// ByteRelay endpoint over a connected QTcpSocket.
class TcpSocketEndpoint final : public ByteRelayEndpoint {
public:
    explicit TcpSocketEndpoint(QTcpSocket* socket) : m_socket(socket) {}

    qint64 bytesAvailable() const override { return m_socket->bytesAvailable(); }
    QByteArray read(qint64 maxBytes) override { return m_socket->read(maxBytes); }
    qint64 write(const QByteArray& data) override
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState) {
            return -1;
        }
        return m_socket->write(data);
    }
    qint64 bytesToWrite() const override { return m_socket->bytesToWrite(); }
    void closeGracefully() override { m_socket->disconnectFromHost(); }
    void abort() override { m_socket->abort(); }

private:
    QTcpSocket* m_socket;
};

} // namespace AetherSDR
