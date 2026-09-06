#include "ControlSession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QPointer>
#include <QThread>
#include <QUuid>

#include <utility>

namespace AetherSDR::control {

Q_LOGGING_CATEGORY(lcControlSession, "aether.control.session")

ControlSession::ControlSession(ControlResourceStore* resources,
                               qint64 maxQueuedOutputBytes,
                               SessionAuthorization authorization,
                               QObject* parent)
    : QObject(parent),
      m_resources(resources),
      m_authorization(authorization),
      m_maxQueuedOutputBytes(maxQueuedOutputBytes)
{
    Q_ASSERT(m_resources);
    connect(m_resources, &ControlResourceStore::resourceChanged,
            this, &ControlSession::onResourceChanged);
    connect(m_resources, &ControlResourceStore::resourceRemoved,
            this, &ControlSession::onResourceRemoved);
}

bool ControlSession::isAuthenticated() const
{
    return !m_revoked
        && (m_authorization == SessionAuthorization::Observer
            || m_authorization == SessionAuthorization::AuthenticatedWithoutGrants);
}

bool ControlSession::canObserve() const
{
    return isAuthenticated() && isNegotiated()
        && m_authorization == SessionAuthorization::Observer;
}

void ControlSession::completeNegotiation()
{
    Q_ASSERT(isAuthenticated() && !isNegotiated());
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

std::optional<ProtocolError> ControlSession::observationError() const
{
    if (m_revoked) {
        return ProtocolError{QStringLiteral("auth.invalid"),
                             QStringLiteral("session authorization was revoked"), {}, false};
    }
    if (!isAuthenticated()) {
        return ProtocolError{QStringLiteral("auth.required"),
                             QStringLiteral("authenticated transport context required"), {}, false};
    }
    if (!isNegotiated()) {
        return ProtocolError{QStringLiteral("session.invalid"),
                             QStringLiteral("session has not negotiated"), {}, false};
    }
    if (!canObserve()) {
        return ProtocolError{QStringLiteral("auth.grant_denied"),
                             QStringLiteral("observe grant required"), {}, false};
    }
    return std::nullopt;
}

void ControlSession::revokeAuthorization()
{
    if (m_revoked) {
        return;
    }
    m_revoked = true;
    m_subscriptions.clear();
    m_selectorsByType.clear();
    m_pending.clear();
    m_pendingBytes = 0;
    emit authorizationRevoked();
}

void ControlSession::bindOutputTransport(
    QObject* transportContext,
    std::function<bool(const QByteArray&)> writeFrame,
    std::function<void()> abortTransport)
{
    // Reject invalid bindings in release builds too. In particular, never
    // compensate for mismatched affinity by aborting a socket on another thread.
    if (QThread::currentThread() != thread()
        || !transportContext || transportContext->thread() != thread()) {
        qCWarning(lcControlSession) << "Output transport must bind on the session's owning thread";
        return;
    }
    if (!writeFrame || !abortTransport || m_outputTransportBound) {
        qCWarning(lcControlSession) << "Output transport requires callbacks and may only bind once";
        return;
    }
    m_outputTransportBound = true;
    const QPointer<ControlSession> session(this);
    const QPointer<QObject> transport(transportContext);
    connect(this, &ControlSession::outputReady, transportContext,
            [session, transport, writeFrame = std::move(writeFrame)] {
                if (!session) {
                    return;
                }
                const QList<QByteArray> frames = session->takePendingFrames();
                for (const QByteArray& frame : frames) {
                    // A write can revoke the session or destroy either endpoint.
                    if (!session || !transport || !session->canObserve() || !writeFrame(frame)) {
                        return;
                    }
                }
            }, Qt::QueuedConnection);
    connect(this, &ControlSession::outputOverflow,
            transportContext, abortTransport, Qt::QueuedConnection);
    // Same owning thread: do not defer the abort behind an already queued flush.
    connect(this, &ControlSession::authorizationRevoked,
            transportContext, abortTransport);
    if (isRevoked()) {
        abortTransport();
    }
}

std::optional<ProtocolError> ControlSession::subscribe(
    const QList<ResourceSelector>& selectors, QJsonObject* result)
{
    if (const std::optional<ProtocolError> error = observationError()) {
        return error;
    }
    if (!result || selectors.isEmpty()) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("resources must be a non-empty array"), {}, false};
    }
    if (m_subscriptions.size() >= kMaxSubscriptions) {
        return ProtocolError{QStringLiteral("transport.limit_exceeded"),
                             QStringLiteral("maximum subscriptions reached"), {}, false};
    }

    if (m_resyncRequired) {
        // A fresh atomic baseline supersedes an undrained resync notice. Do not
        // deliver that older invalidation after the successful subscribe reply.
        m_pending.clear();
        m_pendingBytes = 0;
    }
    const QString subscriptionId = QStringLiteral("sub-%1").arg(m_nextSubscription++);
    m_subscriptions.insert(subscriptionId, selectors);
    rebuildSelectorIndex();
    m_resyncRequired = false;

    QJsonArray resources;
    const QList<ResourceSnapshot> snapshots = m_resources->snapshot(selectors);
    for (const ResourceSnapshot& snapshot : snapshots) {
        resources.append(snapshot.toJson());
    }
    *result = {{QStringLiteral("subscription"), subscriptionId},
               {QStringLiteral("sequence"), static_cast<qint64>(m_drainedSequence)},
               {QStringLiteral("resources"), resources}};
    return std::nullopt;
}

std::optional<ProtocolError> ControlSession::unsubscribe(
    const QString& subscriptionId, QJsonObject* result)
{
    if (const std::optional<ProtocolError> error = observationError()) {
        return error;
    }
    if (!result || subscriptionId.isEmpty()) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("subscription must be a non-empty string"),
                             {}, false};
    }
    if (m_subscriptions.remove(subscriptionId) == 0) {
        return ProtocolError{QStringLiteral("resource.not_found"),
                             QStringLiteral("subscription does not exist"), {}, false};
    }
    rebuildSelectorIndex();
    for (qsizetype index = m_pending.size(); index > 0; --index) {
        const PendingMessage& pending = m_pending.at(index - 1);
        if (pending.resource && !observes(*pending.resource)) {
            m_pendingBytes -= pending.frame.size();
            m_pending.removeAt(index - 1);
        }
    }
    *result = {{QStringLiteral("subscription"), subscriptionId},
               {QStringLiteral("removed"), true}};
    return std::nullopt;
}

QList<QByteArray> ControlSession::takePendingFrames()
{
    if (!canObserve()) {
        // Defence in depth: authorization is immutable and revocation already
        // clears this queue. Do not retain charged bytes if that invariant changes.
        m_pending.clear();
        m_pendingBytes = 0;
        return {};
    }
    QList<QByteArray> frames;
    frames.reserve(m_pending.size());
    for (const PendingMessage& pending : std::as_const(m_pending)) {
        frames.append(pending.frame);
        if (pending.sequence > m_drainedSequence) {
            m_drainedSequence = pending.sequence;
        }
    }
    m_pending.clear();
    m_pendingBytes = 0;
    return frames;
}

void ControlSession::rebuildSelectorIndex()
{
    m_selectorsByType.clear();
    for (auto it = m_subscriptions.constBegin(); it != m_subscriptions.constEnd(); ++it) {
        for (const ResourceSelector& selector : it.value()) {
            m_selectorsByType[selector.type].append(selector);
        }
    }
}

bool ControlSession::observes(const ResourceAddress& address) const
{
    // Defence in depth: denied sessions cannot subscribe, and revocation clears
    // the selector index. No supported state currently relies on this guard alone.
    if (!canObserve()) {
        return false;
    }
    const auto bucket = m_selectorsByType.constFind(address.type);
    if (bucket == m_selectorsByType.constEnd()) {
        return false;
    }
    for (const ResourceSelector& selector : *bucket) {
        if (selector.matches(address)) {
            return true;
        }
    }
    return false;
}

void ControlSession::onResourceChanged(const ResourceSnapshot& snapshot)
{
    if (!m_resyncRequired && observes(snapshot.resource)) {
        enqueueResourceEvent(QStringLiteral("resource.changed"),
                             snapshot.resource, snapshot.revision, snapshot.value);
    }
}

void ControlSession::onResourceRemoved(
    const ResourceAddress& address, quint64 revision)
{
    if (!m_resyncRequired && observes(address)) {
        enqueueResourceEvent(QStringLiteral("resource.removed"), address, revision);
    }
}

void ControlSession::enqueueResourceEvent(
    const QString& event, const ResourceAddress& address,
    quint64 revision, const QJsonObject& value)
{
    QJsonObject message{{QStringLiteral("v"), 1},
                        {QStringLiteral("sessionId"), m_sessionId},
                        {QStringLiteral("event"), event},
                        {QStringLiteral("sequence"), static_cast<qint64>(++m_sequence)},
                        {QStringLiteral("resource"), address.toJson()},
                        {QStringLiteral("revision"), static_cast<qint64>(revision)}};
    if (event == QStringLiteral("resource.changed")) {
        message.insert(QStringLiteral("value"), value);
    }
    enqueueCoalesced(address, message);
}

void ControlSession::enqueueCoalesced(
    const ResourceAddress& address, const QJsonObject& message)
{
    const QString key = address.key();
    const QByteArray frame = encodeFrame(message);
    const qint64 bytes = frame.size();
    const quint64 sequence = m_sequence;
    for (qsizetype index = 0; index < m_pending.size(); ++index) {
        const PendingMessage& pending = m_pending.at(index);
        if (pending.coalesceKey == key) {
            const qint64 nextBytes = m_pendingBytes - pending.frame.size() + bytes;
            if (nextBytes > m_maxQueuedOutputBytes) {
                requireResync();
                return;
            }
            m_pendingBytes = nextBytes;
            m_pending.removeAt(index);
            m_pending.append(PendingMessage{key, address, frame, sequence});
            emit outputReady();
            return;
        }
    }

    if (m_pendingBytes + bytes > m_maxQueuedOutputBytes) {
        requireResync();
        return;
    }
    m_pending.append(PendingMessage{key, address, frame, sequence});
    m_pendingBytes += bytes;
    emit outputReady();
}

void ControlSession::requireResync()
{
    if (m_resyncRequired) {
        return;
    }
    m_resyncRequired = true;
    m_subscriptions.clear();
    rebuildSelectorIndex();
    m_pending.clear();
    m_pendingBytes = 0;

    const QJsonObject message{{QStringLiteral("v"), 1},
                              {QStringLiteral("sessionId"), m_sessionId},
                              {QStringLiteral("event"), QStringLiteral("resource.resyncRequired")},
                              {QStringLiteral("sequence"), static_cast<qint64>(++m_sequence)},
                              {QStringLiteral("subscriptionsInvalidated"), true}};
    const QByteArray frame = encodeFrame(message);
    if (frame.size() > m_maxQueuedOutputBytes) {
        emit outputOverflow();
        return;
    }
    m_pending.append(PendingMessage{QString(), std::nullopt, frame, m_sequence});
    m_pendingBytes = frame.size();
    emit outputReady();
}

QByteArray ControlSession::encodeFrame(const QJsonObject& message)
{
    QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact);
    frame.append('\n');
    return frame;
}

} // namespace AetherSDR::control
