#include "core/hfgateway/ModemHostClient.h"

#include <QPointer>
#include <QTcpSocket>

namespace AetherSDR::ModemHost {

namespace {
constexpr int kConnectTimeoutMs = 5000;
constexpr int kCommandReadBufferBytes = 16 * kMaxLineBytes;
}  // namespace

Client::Client(QObject* parent)
    : QObject(parent)
    , m_connectTimeout(this)
{
    m_connectTimeout.setSingleShot(true);
    connect(&m_connectTimeout, &QTimer::timeout, this, [this] {
        onSocketLost(QStringLiteral("timed out connecting to the modem port pair"));
    });
}

Client::~Client()
{
    closeSockets();
}

void Client::connectToModem(const QString& host, quint16 commandPort, quint16 dataPort)
{
    closeSockets();
    m_splitter.reset();
    m_session.reset();

    m_cmd = new QTcpSocket(this);
    m_data = new QTcpSocket(this);
    m_cmd->setReadBufferSize(kCommandReadBufferBytes);
    m_data->setReadBufferSize(kMaxDataBacklogBytes);
    const QPointer<Client> self(this);
    QTcpSocket* const command = m_cmd;
    QTcpSocket* const data = m_data;
    connect(m_cmd, &QTcpSocket::readyRead, this, &Client::onCommandReadyRead);
    connect(data, &QTcpSocket::readyRead, this, [this, data] {
        const QByteArray bytes = data->readAll();
        if (!bytes.isEmpty()) {
            emit dataReceived(bytes);
        }
    });
    for (QTcpSocket* s : {m_cmd, m_data}) {
        connect(s, &QTcpSocket::connected, this, &Client::onSocketConnected);
        connect(s, &QTcpSocket::disconnected, this, [this]() { onSocketLost(QStringLiteral("modem closed the connection")); });
        connect(s, &QTcpSocket::errorOccurred, this, [this, s](QAbstractSocket::SocketError) {
            onSocketLost(s->errorString());
        });
    }
    setState(State::Connecting);
    if (!self || m_cmd != command) {
        return;
    }
    m_connectTimeout.start(kConnectTimeoutMs);
    command->connectToHost(host, commandPort);
    if (self && m_cmd == command) {
        data->connectToHost(host, dataPort);
    }
}

void Client::disconnectFromModem()
{
    closeSockets();
    m_session.reset();
    setState(State::Disconnected);
}

bool Client::send(Request request, const QStringList& callsigns)
{
    if (m_state != State::Connected || !m_cmd) {
        return false;
    }
    const QByteArray bytes = m_session.request(request, callsigns);
    if (bytes.isEmpty()) {
        if (Session::isStop(request) && m_session.refusal() == Refusal::QueueFull) {
            // The modem has left kHardMaxOutstanding commands unanswered and
            // can't even be told to stop: fail closed.
            onSocketLost(QStringLiteral("modem is not answering commands; disconnected"));
        }
        return false;
    }
    const QPointer<Client> self(this);
    QTcpSocket* const command = m_cmd;
    if (command->write(bytes) != bytes.size()) {
        if (self && m_cmd == command) {
            onSocketLost(QStringLiteral("could not send the modem command; disconnected"));
        }
        return false;
    }
    return self && m_cmd == command;
}

bool Client::writeData(const QByteArray& bytes)
{
    if (m_state != State::Connected || !m_data || !m_session.mayWriteData()) {
        return false;
    }
    if (m_data->bytesToWrite() + bytes.size() > kMaxDataBacklogBytes) {
        return false;
    }
    return m_data->write(bytes) == bytes.size();
}

void Client::setState(State s)
{
    if (s == m_state) {
        return;
    }
    m_state = s;
    emit stateChanged(s);
}

void Client::onSocketConnected()
{
    if (m_cmd && m_data && m_cmd->state() == QAbstractSocket::ConnectedState
        && m_data->state() == QAbstractSocket::ConnectedState) {
        m_connectTimeout.stop();
        setState(State::Connected);
    }
}

void Client::onSocketLost(const QString& why)
{
    if (m_state == State::Disconnected) {
        return;
    }
    // Either socket going away ends the connection: a modem reachable on one
    // port but not the other is not usable, and must fail closed.
    const QPointer<Client> self(this);
    disconnectFromModem();
    if (self && m_state == State::Disconnected) {
        emit problem(why);
    }
}

void Client::onCommandReadyRead()
{
    if (!m_cmd) {
        return;
    }
    const QPointer<Client> self(this);
    QTcpSocket* const command = m_cmd;
    const QList<Line> lines = m_splitter.feed(command->readAll());
    for (const Line& line : lines) {
        if (line.rejected) {
            if (m_splitter.rejectedLines() >= kMaxRejectedLines) {
                onSocketLost(QStringLiteral("too many malformed lines from the modem; disconnected"));
                return;
            }
            continue;
        }
        const Session::Event ev = m_session.onLine(line.text);
        switch (ev.kind) {
        case Session::Event::Kind::Reply:
            emit replied(ev.request, ev.ok, ev.version);
            break;
        case Session::Event::Kind::Notification:
            emit notified(ev.notification);
            break;
        case Session::Event::Kind::Unmatched:
            emit problem(QStringLiteral("reply with no command outstanding: %1").arg(ev.notification.raw));
            break;
        }
        if (!self || m_cmd != command) {
            return;  // a slot may close or replace this connection
        }
    }
}

void Client::closeSockets()
{
    m_connectTimeout.stop();
    for (QTcpSocket** p : {&m_cmd, &m_data}) {
        if (QTcpSocket* s = *p) {
            *p = nullptr;
            s->disconnect(this);
            s->abort();
            s->deleteLater();
        }
    }
}

}  // namespace AetherSDR::ModemHost
