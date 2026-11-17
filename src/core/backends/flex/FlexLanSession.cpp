#include "FlexLanSession.h"

#include "FlexStatusIngress.h"
#include "PanadapterStream.h"
#include "RadioConnection.h"

#include <QPointer>

namespace AetherSDR {

FlexLanSession::FlexLanSession(RadioConnection* connection, PanadapterStream* stream,
                             FlexStatusIngress* ingress, QObject* parent)
    : QObject(parent), m_connection(connection), m_stream(stream), m_ingress(ingress),
      m_epoch(std::make_shared<std::atomic<quint64>>(0))
{
    m_commands.bindLan(connection);
    // Capture on the producer thread, then deliver over exactly one queued hop,
    // just like status ingress. Reading the epoch at delivery would relabel an
    // old event with a replacement session's identity.
    connect(connection, &RadioConnection::connected, this, [this, connection] {
        const ConnectionSession state{connection->lineEpoch(), connection->clientHandle(),
            connection->radioAddress(), connection->localAddress(), connection->localTcpPort()};
        const bool synthetic = connection->isSyntheticDemo();
        QMetaObject::invokeMethod(this, [this, state, synthetic] {
            if (!current(state.epoch) || m_connected) { return; }
            m_snapshot = state;
            m_synthetic = synthetic;
            m_connected = true;
            emit connected();
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(connection, &RadioConnection::disconnected, this, [this, connection] {
        const quint64 epoch = connection->lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch] { finish(epoch); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(connection, &RadioConnection::errorOccurred, this, [this, connection](const QString& message) {
        const quint64 epoch = connection->lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch, message] {
            if (!current(epoch)) { return; }
            const QPointer<FlexLanSession> alive(this);
            emit errorOccurred(message);
            // A refusal before H has no disconnected socket signal. Drain it
            // here as well; the desktop or headless owner chooses retry policy.
            if (alive && current(epoch)) { disconnectRadio(); }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(connection, &RadioConnection::versionReceived, this, [this, connection](const QString& version) {
        const quint64 epoch = connection->lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch, version] {
            if (current(epoch)) { emit versionReceived(version); }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
}

FlexLanSession::~FlexLanSession()
{
    m_commands.invalidate();
    m_epoch->store(0);
    QObject::disconnect(m_connection, nullptr, this, nullptr);
    QObject::disconnect(m_connection, nullptr, &m_commands, nullptr);
    waitForCleanup();
    // Ordinary writes are retired before the worker drain. The old transport's
    // keepalive must also be acknowledged off before its socket is closed.
    const quint32 keepaliveSequence = m_commands.allocateSequence();
    QMetaObject::invokeMethod(m_connection, [connection = m_connection, keepaliveSequence] {
        if (connection->isConnected()) {
            connection->gracefulDisconnect(connection->clientHandle(), {}, 0, keepaliveSequence);
        }
    }, Qt::BlockingQueuedConnection);
}

bool FlexLanSession::current(quint64 epoch) const
{
    return m_active && epoch != 0 && m_epoch->load() == epoch;
}

void FlexLanSession::connectRadio(const RadioConnectRequest& request)
{
    if (m_active) { return; } // one intent owns the transport until it is retired
    m_request = request;
    m_snapshot.reset();
    m_connected = false;
    m_active = true;
    const quint64 epoch = m_ingress->beginSession();
    m_epoch->store(epoch);
    const QPointer<FlexLanSession> alive(this);
    m_commands.begin(epoch);
    if (!alive || !current(epoch)) { return; }
    emit starting(epoch);
    if (!alive || !current(epoch)) { return; }
    startTransport(epoch);
}

void FlexLanSession::startTransport(quint64 epoch)
{
    // Cleanup pumps nested Qt events. A queued dial alone could therefore run
    // inside the old close and then be closed by it when that close resumes.
    if (!current(epoch) || m_cleanup) { return; }
    const RadioConnectRequest& request = m_request;
    RadioInfo info;
    info.address = QHostAddress(request.host);
    info.port = request.port;
    info.serial = request.serial;
    info.version = request.version;
    info.sessionBindAddress = request.localBindAddress;
    info.bindSettings.mode = request.requireExplicitLocalBind ? RadioBindMode::Explicit : RadioBindMode::Auto;
    info.bindSettings.bindAddress = request.explicitLocalBindAddress;
    QMetaObject::invokeMethod(m_connection, [connection = m_connection, token = m_epoch, epoch, info] {
        if (token->load() != epoch) { return; }
        connection->setLineEpoch(epoch);
        connection->connectToRadio(info);
    }, Qt::QueuedConnection);
}

void FlexLanSession::stopReceiveTransport()
{
    QMetaObject::invokeMethod(m_stream, [stream = m_stream] {
        stream->stop();
        stream->clearRegisteredStreams();
    }, Qt::BlockingQueuedConnection);
}

void FlexLanSession::disconnectRadio(const RadioDisconnectRequest& request, quint32 sequence,
                                    quint32 keepaliveSequence)
{
    const bool wait = request.completion == RadioDisconnectRequest::Completion::Wait;
    if (!m_active) {
        if (wait) { waitForCleanup(); }
        return;
    }
    const quint64 epoch = m_epoch->exchange(0);
    m_commands.invalidate();
    const quint32 handle = m_snapshot ? m_snapshot->clientHandle : 0;
    m_ingress->retire(epoch);
    stopReceiveTransport();
    m_active = false;
    m_connected = false;
    m_snapshot.reset();
    // A pending close owns the old socket. Canceling an intent waiting behind
    // it must not enqueue another close against a later replacement session.
    if (!m_cleanup) {
        const auto cleanup = std::make_shared<QSemaphore>();
        m_cleanup = cleanup;
        QMetaObject::invokeMethod(m_connection, [this, connection = m_connection, cleanup, handle, request, sequence, keepaliveSequence] {
            if (connection->isConnected()) {
                const QString streamId = request.receiveStreamId && sequence
                    ? QString::number(request.receiveStreamId, 16) : QString();
                connection->gracefulDisconnect(handle, streamId, sequence, keepaliveSequence);
            } else {
                connection->disconnectFromRadio();
            }
            QMetaObject::invokeMethod(this, [this, cleanup] {
                if (m_cleanup != cleanup) { return; }
                m_cleanup.reset();
                startTransport(m_epoch->load());
            }, Qt::QueuedConnection);
            // The destructor waits on this gate, keeping the receiver alive
            // through the completion post. No receiver access follows release.
            cleanup->release();
        }, Qt::QueuedConnection);
    }
    if (wait) { waitForCleanup(); }
    const QPointer<FlexLanSession> alive(this);
    emit disconnected();
    if (alive && m_commands.epoch() == epoch) { m_commands.retire(QStringLiteral("Connection closed.")); }
}

void FlexLanSession::waitForCleanup()
{
    if (!m_cleanup) { return; }
    // A worker queue barrier can run inside gracefulDisconnect's nested event
    // loop. Only the completion gate proves the entire close has returned.
    m_cleanup->acquire();
    m_cleanup.reset();
}

void FlexLanSession::finish(quint64 epoch)
{
    if (!current(epoch)) { return; }
    m_epoch->store(0);
    m_commands.invalidate();
    m_ingress->retire(epoch);
    stopReceiveTransport();
    m_active = false;
    m_connected = false;
    m_snapshot.reset();
    const QPointer<FlexLanSession> alive(this);
    emit disconnected();
    if (alive && m_commands.epoch() == epoch) { m_commands.retire(QStringLiteral("Connection closed.")); }
}

ReceiveTransportResult FlexLanSession::startReceiveTransport(bool rebind)
{
    if (!m_connected || !m_snapshot) { return {}; }
    const quint64 epoch = m_snapshot->epoch;
    const FlexLanEndpoint endpoint{m_snapshot->remoteAddress, m_snapshot->localAddress,
        m_request.localBindAddress, m_request.explicitLocalBindAddress,
        m_request.requireExplicitLocalBind, m_synthetic};
    ReceiveTransportResult result;
    QMetaObject::invokeMethod(m_stream, [stream = m_stream, token = m_epoch, epoch, endpoint, rebind, &result] {
        if (token->load() != epoch) { return; }
        result.ready = rebind ? stream->rebindToEphemeralPort(endpoint) : stream->start(endpoint);
        result.localPort = result.ready ? stream->localPort() : 0;
    }, Qt::BlockingQueuedConnection);
    return result;
}

} // namespace AetherSDR
