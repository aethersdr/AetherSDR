#include "FlexWanSession.h"
#include "FlexStatusIngress.h"
#include "PanadapterStream.h"
#include <QPointer>
#include <QEventLoop>
#include <utility>

namespace AetherSDR {
FlexWanSession::FlexWanSession(PanadapterStream* stream, FlexStatusIngress* ingress, QObject* parent)
    : QObject(parent), m_stream(stream), m_ingress(ingress)
{
    m_prepareReceive = [stream](const WanRadioInfo& radio) {
        ReceiveTransportResult result;
        QMetaObject::invokeMethod(stream, [stream, radio, &result] {
            result.ready = stream->startWan(QHostAddress(radio.publicIp), quint16(radio.publicUdpPort));
            result.localPort = result.ready ? stream->localPort() : 0;
        }, Qt::BlockingQueuedConnection);
        return result;
    };
    m_stopReceive = [stream] {
        QMetaObject::invokeMethod(stream, [stream] { stream->stop(); stream->clearRegisteredStreams(); },
                                  Qt::BlockingQueuedConnection);
    };
    m_dial = [this](const QString& host, quint16 port, const QString& handle) {
        m_connection.connectToRadio(host, port, handle);
    };
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_closing) { finish(m_epoch); }
        else { fail(m_epoch, tr("SmartLink radio connection timed out.")); }
    });
    // Queue lifecycle on the same owner loop as status ingress. Capture its
    // originating epoch before posting, never when the event is delivered.
    connect(&m_connection, &WanConnection::connected, this, [this] {
        const ConnectionSession state{m_connection.lineEpoch(), m_connection.clientHandle(),
            m_connection.radioAddress(), {}, m_connection.localTcpPort(), ConnectionSession::Transport::Wan};
        QMetaObject::invokeMethod(this, [this, state] {
            if (!current(state.epoch)) { return; }
            m_deadline.stop();
            m_snapshot = state;
            emit connected();
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(&m_connection, &WanConnection::disconnected, this, [this] {
        const quint64 epoch = m_connection.lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch] { finish(epoch); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(&m_connection, &WanConnection::errorOccurred, this, [this](const QString& message) {
        const quint64 epoch = m_connection.lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch, message] { fail(epoch, message); }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(&m_connection, &WanConnection::versionReceived, this, [this](const QString& version) {
        const quint64 epoch = m_connection.lineEpoch();
        QMetaObject::invokeMethod(this, [this, epoch, version] {
            if (current(epoch)) { emit versionReceived(version); }
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
    connect(&m_connection, &WanConnection::pingRttMeasured, this, [this](int milliseconds) {
        if (current(m_epoch)) { emit pingMeasured(milliseconds); }
    });
    connect(&m_connection, &WanConnection::certFingerprintMismatch, this,
        [this](const QString& host, const QString& expected, const QString& presented) {
            if (!current(m_epoch)) { return; }
            m_certificateAttempt = m_epoch;
            emit certificateRequired(m_epoch, host, expected, presented);
        });
}

FlexWanSession::~FlexWanSession()
{
    QObject::disconnect(this, nullptr, nullptr, nullptr);
    if (!m_finishing) { disconnectRadio(); }
    QObject::disconnect(&m_connection, nullptr, this, nullptr);
    m_deadline.stop();
    if (m_epoch) { m_ingress->retire(m_epoch); stopReceive(); }
    m_connection.abortConnection();
    if (m_request.account) { m_request.account->cancelConnection(m_lease); }
}

void FlexWanSession::connectRadio(const RemoteRadioRequest& request)
{
    if (active()) { return; }
    m_request = request;
    m_snapshot.reset();
    m_closing = false;
    m_certificateAttempt = 0;
    m_epoch = m_ingress->bindLive(&m_connection);
    const quint64 epoch = m_epoch;
    const QPointer<FlexWanSession> alive(this);
    emit starting(epoch);
    if (!alive || !current(epoch)) { return; }
    const WanRadioInfo& radio = request.radio;
    const QHostAddress address(radio.publicIp);
    if (!request.account || address.isNull() || address.isMulticast()
        || radio.publicTlsPort <= 0 || radio.publicTlsPort > 65535
        || radio.publicUdpPort <= 0 || radio.publicUdpPort > 65535) {
        fail(epoch, tr("SmartLink radio has no usable forwarded TLS/UDP endpoint."));
        return;
    }
    m_deadline.start(45000);
    const ReceiveTransportResult transport = m_prepareReceive(radio);
    m_udpPort = transport.localPort;
    if (!transport.ready || !m_udpPort) { fail(epoch, tr("SmartLink UDP setup failed.")); return; }
    m_lease = request.account->beginConnection(radio.serial, m_udpPort, request.disconnectHandles,
        [alive, epoch](quint64 lease, const QString& handle) {
            if (!alive) { return; }
            QMetaObject::invokeMethod(alive, [alive, epoch, lease, handle] {
                if (!alive || !alive->current(epoch) || alive->m_lease != lease) { return; }
                const WanRadioInfo radio = alive->m_request.radio;
                alive->m_dial(radio.publicIp, quint16(radio.publicTlsPort), handle);
            }, Qt::QueuedConnection);
        }, [alive, epoch](quint64 lease, const QString& message) {
            if (!alive) { return; }
            QMetaObject::invokeMethod(alive, [alive, epoch, lease, message] {
                if (alive && alive->m_lease == lease) { alive->fail(epoch, message); }
            }, Qt::QueuedConnection);
        });
    if (!m_lease) { fail(epoch, tr("SmartLink account is unavailable or already in use.")); }
}

void FlexWanSession::stopReceive()
{
    m_stopReceive();
    m_udpPort = 0;
}

void FlexWanSession::disconnectRadio(const RadioDisconnectRequest& request)
{
    if (!active()) { return; }
    const quint64 epoch = m_epoch;
    const QPointer<FlexWanSession> alive(this);
    if (m_closing) {
        if (request.completion == RadioDisconnectRequest::Completion::Wait) { waitForClose(epoch); }
        return;
    }
    m_closing = true;
    commands().beginCleanup();
    m_certificateAttempt = 0;
    m_ingress->retire(epoch);
    stopReceive();
    m_deadline.start(1000);
    if (!m_connection.isConnected()) {
        closeSocket(epoch);
        if (alive && request.completion == RadioDisconnectRequest::Completion::Wait) { waitForClose(epoch); }
        return;
    }
    FlexCommandSession::Options options;
    options.cleanup = true;
    options.timeoutMs = 350;
    const auto removeStream = [alive, epoch, request, options](const FlexCommandSession::Reply&) {
        if (!alive || alive->m_epoch != epoch || !alive->m_closing) { return; }
        if (request.receiveStreamId) {
            alive->commands().submit(QStringLiteral("stream remove 0x%1").arg(request.receiveStreamId, 0, 16),
                options, [alive, epoch](const FlexCommandSession::Reply&) {
                    if (alive) { alive->closeSocket(epoch); }
                });
        } else { alive->closeSocket(epoch); }
    };
    if (!commands().submit(QStringLiteral("keepalive disable"), options, removeStream)) {
        closeSocket(epoch);
    }
    if (alive && request.completion == RadioDisconnectRequest::Completion::Wait) { waitForClose(epoch); }
}

void FlexWanSession::waitForClose(quint64 epoch)
{
    if (m_epoch != epoch || !m_closing) { return; }
    // TLS belongs to this loop: keep its reply/timer delivery running while
    // normal shutdown drains. The closing epoch refuses replacement writes.
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(this, &FlexWanSession::disconnected, &loop, &QEventLoop::quit);
    connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QPointer<FlexWanSession> alive(this);
    deadline.start(1100);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (alive && m_epoch == epoch && m_closing) { finish(epoch); }
}

void FlexWanSession::closeSocket(quint64 epoch)
{
    if (m_epoch != epoch || !epoch) { return; }
    m_connection.disconnectFromRadio();
    if (m_connection.isSocketIdle()) { finish(epoch); }
}

void FlexWanSession::finish(quint64 epoch)
{
    if (!epoch || m_epoch != epoch || m_finishing) { return; }
    m_finishing = true;
    m_closing = true;
    const QPointer<FlexWanSession> alive(this);
    const auto account = std::exchange(m_request.account, {});
    const quint64 lease = std::exchange(m_lease, 0);
    m_deadline.stop();
    m_snapshot.reset();
    m_certificateAttempt = 0;
    m_ingress->retire(epoch);
    stopReceive();
    if (account) { account->cancelConnection(lease); }
    if (!alive) { return; }
    m_connection.abortConnection();
    if (!alive) { return; }
    m_epoch = 0;
    m_closing = false;
    m_finishing = false;
    emit disconnected();
}

void FlexWanSession::fail(quint64 epoch, const QString& message)
{
    if (!current(epoch)) { return; }
    const QPointer<FlexWanSession> alive(this);
    emit errorOccurred(message);
    if (alive && current(epoch)) { disconnectRadio(); }
}

ReceiveTransportResult FlexWanSession::startReceiveTransport()
{
    if (!isConnected() || !m_udpPort) { return {}; }
    const quint32 handle = m_snapshot->clientHandle;
    QMetaObject::invokeMethod(m_stream, [stream = m_stream, handle] { stream->startWanUdpRegister(handle); },
                              Qt::BlockingQueuedConnection);
    return {true, m_udpPort};
}

void FlexWanSession::decideCertificate(quint64 attempt, bool accept)
{
    if (!current(attempt) || m_certificateAttempt != attempt) { return; }
    m_certificateAttempt = 0;
    if (accept) { m_connection.acceptPresentedCert(); }
    else { m_connection.rejectPresentedCert(); }
}
} // namespace AetherSDR
