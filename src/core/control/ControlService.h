#pragma once

#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"
#include "ControlSession.h"
#include "RadioConnectionTarget.h"

#include <QJsonObject>
#include <QString>
#include <QPointer>

namespace AetherSDR::control {

struct ServiceReply {
    QJsonObject message;
    bool closeAfterWrite{false};
};

// Transport-neutral Stage-3 service kernel. The current surface is strictly
// typed reads/subscriptions plus optional non-TX connection intents. A session's
// trusted transport context supplies authorization; hello cannot grant it.
class ControlService final {
public:
    explicit ControlService(ControlResourceStore* resources,
                            RadioConnectionTarget* connectionTarget = nullptr);

    // Trusted startup only: bind once, on the owning thread, before dispatch.
    // This lets the daemon claim its endpoint before constructing any models.
    [[nodiscard]] bool bindConnectionTarget(RadioConnectionTarget* target);

    [[nodiscard]] ServiceReply handle(
        const QByteArray& bytes, ControlSession* session) const;

    [[nodiscard]] QJsonObject capabilities(const ControlSession& session) const;

private:
    [[nodiscard]] static ServiceReply failure(
        const QString& id, ProtocolError error, bool closeAfterWrite = false);
    [[nodiscard]] static std::optional<ProtocolError> validateHelloParams(
        const QJsonObject& params);
    [[nodiscard]] static bool acceptsVersionOne(const QJsonObject& params);
    [[nodiscard]] ServiceReply handleConnection(
        const ProtocolRequest& request, const ControlSession& session) const;

    ControlResourceStore* m_resources{nullptr};
    QPointer<RadioConnectionTarget> m_connectionTarget;
    bool m_targetBound{false};
    mutable bool m_dispatchStarted{false};
};

} // namespace AetherSDR::control
