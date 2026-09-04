#pragma once

#include "ControlProtocolCodec.h"

#include <QJsonObject>
#include <QString>

namespace AetherSDR::control {

struct ControlSessionState {
    QString sessionId;
    bool negotiated{false};
};

struct ServiceReply {
    QJsonObject message;
    bool closeAfterWrite{false};
};

// Transport-neutral Stage-3 service kernel. The first landed surface is
// intentionally observe-only: negotiation and capability discovery. Model
// resources and non-TX control methods attach here in subsequent slices.
class ControlService final {
public:
    [[nodiscard]] ServiceReply handle(
        const QByteArray& bytes, ControlSessionState* session) const;

    [[nodiscard]] static QJsonObject capabilities(const ControlSessionState& session);

private:
    [[nodiscard]] static ServiceReply failure(
        const QString& id, ProtocolError error, bool closeAfterWrite = false);
    [[nodiscard]] static std::optional<ProtocolError> validateHelloParams(
        const QJsonObject& params);
    [[nodiscard]] static bool acceptsVersionOne(const QJsonObject& params);
};

} // namespace AetherSDR::control
