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
};

} // namespace AetherSDR::control
