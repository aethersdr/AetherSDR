#pragma once

#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"
#include "ControlCredentials.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

namespace AetherSDR::control {

// Supplied by trusted embedding/transport code, never decoded from hello.
// The local endpoint supplies Observer after enforcing current-user access.
// Control remains independent of observation; transmit is not representable.
enum class SessionAuthorization {
    Unauthenticated,
    AuthenticatedWithoutGrants,
    Observer,
    Controller,
    ObserverController,
};

// Per-client protocol state. Resource events are session-sequenced and held in
// a bounded, coalescing queue until the transport drains them.
class ControlSession final : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxSubscriptions = 64;
    static constexpr int kRequestsPerSecond = 100;
    static constexpr int kRequestBurst = 200;

    explicit ControlSession(ControlResourceStore* resources,
                            qint64 maxQueuedOutputBytes,
                            SessionAuthorization authorization = SessionAuthorization::Unauthenticated,
                            QObject* parent = nullptr,
                            std::function<qint64()> monotonicNanoseconds = {});
    ~ControlSession() override;

    [[nodiscard]] const QString& sessionId() const { return m_sessionId; }
    [[nodiscard]] bool isNegotiated() const { return !m_sessionId.isEmpty(); }
    [[nodiscard]] bool isAuthenticated() const;
    [[nodiscard]] bool canObserve() const;
    [[nodiscard]] bool canControl() const;
    // Credential identity and operator administration are separate from the
    // observe/control permissions and from a live, radio-scoped TX grant.
    [[nodiscard]] QString principalId() const;
    [[nodiscard]] bool canAdministerGrants() const;
    // Capture at trusted grant composition, not at worker dispatch time.
    // The returned predicate reads only the original credential's atomic fence.
    [[nodiscard]] std::function<bool()> credentialFence() const;
    [[nodiscard]] bool isRevoked() const { return m_revoked; }
    // Terminal for this session. Clears subscriptions and queued frames before
    // notifying the transport to abort its own output buffer. A new verified
    // connection must construct a new session; hello cannot restore this one.
    void revokeAuthorization();
    // Terminal admission fence, before a final protocol error is written or
    // transport destruction is deferred. Does not discard transport-owned
    // output; explicit revocation additionally aborts that output below.
    void endAuthorization();
    // Trusted engine lifetime wiring, not authentication or grant issuance.
    // Runs once, synchronously, before transport/presentation callbacks. Both
    // endpoints must share this thread; the context bounds callback lifetime
    // and must retire its owned authority before the context is destroyed.
    // A late binding to an ended session is retired immediately.
    [[nodiscard]] bool bindAuthorityLifetime(QObject* context, std::function<void()> retire);

    // Bind once on the owning thread; neither endpoint may move threads afterward.
    // Invalid/repeated binds log and leave the existing wiring unchanged.
    // The transport context bounds callback
    // lifetime; writeFrame accepts a complete frame and returns false on failure.
    // abortTransport must synchronously discard transport-owned output. Keeping
    // this wiring here lets socket-free tests exercise the production lifecycle.
    void bindOutputTransport(QObject* transportContext,
                             std::function<bool(const QByteArray&)> writeFrame,
                             std::function<void()> abortTransport);

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
    void authorizationRevoked();

private:
    friend class ControlService;
    [[nodiscard]] bool bindPrincipal(const ControlCredentials::Principal& principal);
    void completeNegotiation();
    [[nodiscard]] bool consumeRequest();
    [[nodiscard]] std::optional<ProtocolError> observationError() const;

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
    const SessionAuthorization m_authorization;
    ControlCredentials::Principal m_principal;
    bool m_principalBound{false};
    QString m_sessionId;
    bool m_revoked{false};
    bool m_abortRequested{false};
    bool m_authorityLifetimeBound{false};
    QPointer<QObject> m_authorityContext;
    std::function<void()> m_retireAuthority;
    // Optional trusted clock injection for deterministic boundary tests. Never
    // supplied by the wire; production uses elapsed monotonic nanoseconds.
    std::function<qint64()> m_monotonicNanoseconds;
    QElapsedTimer m_requestClock;
    qint64 m_lastRequestTime{0};
    double m_requestTokens{kRequestBurst};
    bool m_requestLimitExceeded{false};
    bool m_outputTransportBound{false};
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
