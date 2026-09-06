#pragma once

#include "ControlProtocolCodec.h"
#include "ControlResourceStore.h"
#include "ControlSession.h"

#include <QJsonObject>
#include <QString>

namespace AetherSDR::control {

struct ServiceReply {
    QJsonObject message;
    bool closeAfterWrite{false};
};

// Transport-neutral Stage-3 service kernel. The current surface is strictly
// observe-only: negotiation, capability discovery, typed resource reads, and
// subscriptions. A session's trusted transport context supplies authorization;
// hello cannot grant permissions. Non-TX methods attach in a subsequent slice.
class ControlService final {
public:
    explicit ControlService(ControlResourceStore* resources);

    [[nodiscard]] ServiceReply handle(
        const QByteArray& bytes, ControlSession* session) const;

    [[nodiscard]] QJsonObject capabilities(const ControlSession& session) const;

private:
    [[nodiscard]] static ServiceReply failure(
        const QString& id, ProtocolError error, bool closeAfterWrite = false);
    [[nodiscard]] static std::optional<ProtocolError> validateHelloParams(
        const QJsonObject& params);
    [[nodiscard]] static bool acceptsVersionOne(const QJsonObject& params);

    ControlResourceStore* m_resources{nullptr};
};

} // namespace AetherSDR::control
