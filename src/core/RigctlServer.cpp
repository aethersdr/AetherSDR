#include "RigctlServer.h"
#include "LogManager.h"
#include "RigctlProtocol.h"
#include "models/RadioModel.h"

namespace MasterSDR {

RigctlServer::RigctlServer(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{}

RigctlServer::~RigctlServer()
{
    stop();
}

bool RigctlServer::start(quint16 port)
{
    if (m_server)
        return m_server->isListening();

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RigctlServer::onNewConnection);

    if (!m_server->listen(QHostAddress::AnyIPv4, port)) {
        qCWarning(lcCat) << "RigctlServer: failed to listen on port" << port
                    << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    qCInfo(lcCat) << "RigctlServer: listening on port" << m_server->serverPort();
    return true;
}

void RigctlServer::stop()
{
    if (!m_server) return;

    QList<ClientState> clients;
    clients.swap(m_clients);
    emit clientCountChanged(0);

    for (auto& cs : clients) {
        if (cs.socket) {
            // Prevent synchronous disconnected() delivery from re-entering
            // onClientDisconnected() while we're already tearing down clients.
            cs.socket->disconnect(this);
            cs.socket->close();
            cs.socket->deleteLater();
        }
        delete cs.protocol;
    }

    m_server->close();
    delete m_server;
    m_server = nullptr;

    qCInfo(lcCat) << "RigctlServer: stopped";
}

bool RigctlServer::isRunning() const
{
    return m_server && m_server->isListening();
}

quint16 RigctlServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

void RigctlServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto* socket = m_server->nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);  // TCP_NODELAY
        auto* protocol = new RigctlProtocol(m_model);
        protocol->setSliceIndex(m_sliceIndex);

        ClientState cs;
        cs.socket = socket;
        cs.protocol = protocol;
        m_clients.append(cs);

        connect(socket, &QTcpSocket::readyRead, this, &RigctlServer::onClientData);
        connect(socket, &QTcpSocket::disconnected, this, &RigctlServer::onClientDisconnected);

        qCInfo(lcCat) << "RigctlServer: client connected from" << socket->peerAddress().toString();
        emit clientCountChanged(m_clients.size());
    }
}

void RigctlServer::onClientData()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    // Find the client state
    int idx = -1;
    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].socket == socket) { idx = i; break; }
    }
    if (idx < 0) return;

    auto& cs = m_clients[idx];
    cs.buffer.append(socket->readAll());

    // Process complete lines
    while (true) {
        int nlPos = cs.buffer.indexOf('\n');
        if (nlPos < 0) break;

        QString line = QString::fromUtf8(cs.buffer.left(nlPos));
        cs.buffer.remove(0, nlPos + 1);

        // Check for quit
        QString trimmed = line.trimmed();
        if (trimmed == "q" || trimmed == "\\quit") {
            socket->disconnectFromHost();
            return;
        }

        QString response = cs.protocol->handleLine(line);
        if (!response.isEmpty()) {
            qCDebug(lcCat) << "rigctld cmd:" << trimmed
                     << "-> resp:" << response.left(60).trimmed();
            socket->write(response.toUtf8());
        }
    }
}

void RigctlServer::onClientDisconnected()
{
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;

    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].socket == socket) {
            delete m_clients[i].protocol;
            m_clients.removeAt(i);
            break;
        }
    }

    socket->deleteLater();
    qCInfo(lcCat) << "RigctlServer: client disconnected," << m_clients.size() << "remaining";
    emit clientCountChanged(m_clients.size());
}

} // namespace MasterSDR
