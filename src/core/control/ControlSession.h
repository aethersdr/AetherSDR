#pragma once

#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"

#include <QByteArray>
#include <QHash>
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
    // Complete wire frames (compact JSON plus the framing newline), encoded
    // once at enqueue time so neither the queue bound nor the transport has to
    // serialize the message again.
    [[nodiscard]] QList<QByteArray> takePendingFrames();
    [[nodiscard]] quint64 sequence() const { return m_sequence; }
    [[nodiscard]] qint64 maxQueuedOutputBytes() const { return m_maxQueuedOutputBytes; }

signals:
    void outputReady();
    void outputOverflow();

private:
    struct PendingMessage {
        QString coalesceKey;
        std::optional<ResourceAddress> resource;
        QByteArray frame;
        quint64 sequence{0};
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
    void rebuildSelectorIndex();
    [[nodiscard]] static QByteArray encodeFrame(const QJsonObject& message);

    ControlResourceStore* m_resources{nullptr};
    qint64 m_maxQueuedOutputBytes{0};
    quint64 m_sequence{0};
    quint64 m_drainedSequence{0};
    quint64 m_nextSubscription{1};
    QMap<QString, QList<ResourceSelector>> m_subscriptions;
    // Derived from m_subscriptions on every subscription change. observes() runs
    // once per session per store change, so scanning all 64x64 advertised
    // selectors there would put that sweep on the model's publish path; bucketed
    // by resource type it only ever visits selectors that could match.
    QHash<QString, QList<ResourceSelector>> m_selectorsByType;
    QList<PendingMessage> m_pending;
    qint64 m_pendingBytes{0};
    bool m_resyncRequired{false};
};

} // namespace AetherSDR::control
