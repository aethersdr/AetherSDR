#include "ControlSession.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <utility>

namespace AetherSDR::control {

ControlSession::ControlSession(ControlResourceStore* resources,
                               qint64 maxQueuedOutputBytes,
                               QObject* parent)
    : QObject(parent),
      m_resources(resources),
      m_maxQueuedOutputBytes(maxQueuedOutputBytes)
{
    Q_ASSERT(m_resources);
    connect(m_resources, &ControlResourceStore::resourceChanged,
            this, &ControlSession::onResourceChanged);
    connect(m_resources, &ControlResourceStore::resourceRemoved,
            this, &ControlSession::onResourceRemoved);
}

std::optional<ProtocolError> ControlSession::subscribe(
    const QList<ResourceSelector>& selectors, QJsonObject* result)
{
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
                        {QStringLiteral("sessionId"), sessionId},
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
                              {QStringLiteral("sessionId"), sessionId},
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
