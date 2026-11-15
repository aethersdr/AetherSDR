#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>
#include <map>
#include <optional>

namespace AetherSDR::aetherd {

// What the remote-engine backend may send, and when
// (docs/aetherd-remote-backend.md, rule 3). Socket-free: the backend asks for
// the next frame and sends it.
//
//  - A session-wide client token bucket at half the engine's advertised
//    budget charges every frame after welcome: control requests, subscribes,
//    re-reads, retries and resync snapshots.
//  - One request is in flight per resource, because every control request on
//    a resource carries its revision. Pending intents are kept per resource
//    AND method, so the next frequency intent never replaces a mode change;
//    a newer intent for the same method replaces the queued one (latest wins).
//  - Plain frames (subscribe, get) go first, in order.
class RemoteEngineScheduler {
public:
    struct Limits {
        double requestsPerSecond{50.0};
        double requestBurst{50.0};
        bool operator==(const Limits&) const = default;
    };
    // Half of welcome.limits' requestsPerSecond, with a burst of the same size
    // capped at half of requestBurst (50/s, burst 50 at today's 100/200); 50/50
    // when either is missing or not a positive number.
    [[nodiscard]] static Limits clientLimits(const QJsonObject& welcomeLimits);

    struct Frame {
        QString method;
        QJsonObject params;
        // The resource a control intent addresses (its ResourceAddress key);
        // empty for a plain frame. While this frame is unanswered, no other
        // intent for the resource leaves.
        QString resourceKey;
        bool retried{false};
    };

    explicit RemoteEngineScheduler(std::function<qint64()> clockMs);

    void setLimits(Limits limits);
    [[nodiscard]] Limits limits() const { return m_limits; }

    void enqueuePlain(const QString& method, const QJsonObject& params);
    // Replaces any queued intent with the same resource and method.
    void submitIntent(const QString& resourceKey, const QString& method,
                      const QJsonObject& params, bool retried = false);
    // Whether an intent for this resource and method is queued or in flight:
    // the backend holds back the engine's older value for those fields.
    [[nodiscard]] bool intentOutstanding(const QString& resourceKey, const QString& method) const;
    [[nodiscard]] bool hasQueuedIntent(const QString& resourceKey, const QString& method) const;

    // The next frame to send now, taking one token. Nullopt when nothing is
    // eligible or the bucket is empty.
    [[nodiscard]] std::optional<Frame> takeNext();
    // Milliseconds until takeNext() could yield, or nullopt when nothing waits.
    [[nodiscard]] std::optional<qint64> msUntilReady();
    // The answer for an intent on this resource arrived.
    void completed(const QString& resourceKey);

    void clear();
    [[nodiscard]] int queuedCount() const;
    [[nodiscard]] double tokens();

private:
    struct Pending {
        quint64 order{0};
        Frame frame;
    };
    void refill();
    [[nodiscard]] bool eligibleIntentWaiting() const;

    std::function<qint64()> m_clock;
    Limits m_limits;
    double m_tokens{50.0};
    qint64 m_lastRefillMs{0};
    QList<Frame> m_plain;
    // key = resourceKey + '\n' + method
    std::map<QString, Pending> m_intents;
    QSet<QString> m_inFlight;   // resource keys
    QSet<QString> m_inFlightMethods;   // resourceKey + '\n' + method
    quint64 m_order{0};
};

} // namespace AetherSDR::aetherd
