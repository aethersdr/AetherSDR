#pragma once

#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include <optional>

namespace AetherSDR::control {

// Per-client protocol state. Resource events are session-sequenced and held in
// a bounded, coalescing queue until the transport drains them.
class ControlSession final : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxSubscriptions = 64;

    explicit ControlSession(ControlResourceStore* resources,
                            qint64 maxQueuedOutputBytes,
                            QObject* parent = nullptr);

    QString sessionId;
    bool negotiated{false};

    [[nodiscard]] std::optional<ProtocolError> subscribe(
        const QList<ResourceSelector>& selectors, QJsonObject* result);
    [[nodiscard]] std::optional<ProtocolError> unsubscribe(
        const QString& subscriptionId, QJsonObject* result);
    [[nodiscard]] QList<QJsonObject> takePendingMessages();
    [[nodiscard]] quint64 sequence() const { return m_sequence; }
    [[nodiscard]] qint64 maxQueuedOutputBytes() const { return m_maxQueuedOutputBytes; }

signals:
    void outputReady();
    void outputOverflow();

private:
    struct PendingMessage {
        QString coalesceKey;
        std::optional<ResourceAddress> resource;
        QJsonObject message;
        qint64 bytes{0};
    };

    [[nodiscard]] bool observes(const ResourceAddress& address) const;
    void onResourceChanged(const ResourceSnapshot& snapshot);
    void onResourceRemoved(const ResourceAddress& address, quint64 revision);
    void enqueueResourceEvent(const QString& event,
                              const ResourceAddress& address,
                              quint64 revision,
                              const QJsonObject& value = {});
    void enqueueCoalesced(const ResourceAddress& address,
                          const QJsonObject& message);
    void requireResync();
    [[nodiscard]] static qint64 wireBytes(const QJsonObject& message);

    ControlResourceStore* m_resources{nullptr};
    qint64 m_maxQueuedOutputBytes{0};
    quint64 m_sequence{0};
    quint64 m_nextSubscription{1};
    QMap<QString, QList<ResourceSelector>> m_subscriptions;
    QList<PendingMessage> m_pending;
    qint64 m_pendingBytes{0};
    bool m_resyncRequired{false};
};

} // namespace AetherSDR::control
