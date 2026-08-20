#include "ControlService.h"

#include <QJsonArray>
#include <QSet>
#include <QUuid>

#include <cmath>

namespace AetherSDR::control {

ServiceReply ControlService::handle(
    const QByteArray& bytes, ControlSessionState* session) const
{
    const ParseResult parsed = ControlProtocolCodec::parseRequest(bytes);
    if (!parsed.ok()) {
        return failure(parsed.requestId,
                       parsed.error.value_or(ProtocolError{
                           QStringLiteral("protocol.invalid_envelope"),
                           QStringLiteral("request could not be parsed"), {}, false}),
                       !session->negotiated);
    }

    const ProtocolRequest& request = *parsed.request;
    if (!session->negotiated) {
        if (!request.isHello()) {
            return failure(request.id,
                           {QStringLiteral("protocol.invalid_envelope"),
                            QStringLiteral("hello must be the first request"), {}, false},
                           true);
        }
        if (const std::optional<ProtocolError> validationError =
                validateHelloParams(request.params)) {
            return failure(request.id, *validationError, true);
        }
        if (!acceptsVersionOne(request.params)) {
            return failure(request.id,
                           {QStringLiteral("protocol.version_unsupported"),
                            QStringLiteral("no mutually supported protocol version"),
                            {{QStringLiteral("supported"), QJsonArray{1}}}, false},
                           true);
        }

        session->sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        session->negotiated = true;
        return {ControlProtocolCodec::successResponse(
                    request.id, capabilities(*session)), false};
    }

    if (request.isHello()) {
        return failure(request.id,
                       {QStringLiteral("protocol.invalid_envelope"),
                        QStringLiteral("hello may only be sent once"), {}, false});
    }
    if (request.sessionId != session->sessionId) {
        return failure(request.id,
                       {QStringLiteral("session.invalid"),
                        QStringLiteral("session does not belong to this connection"), {}, false});
    }
    if (request.method == QStringLiteral("capabilities.get")) {
        if (!request.params.isEmpty()) {
            return failure(request.id,
                           {QStringLiteral("request.invalid_params"),
                            QStringLiteral("capabilities.get takes no parameters"), {}, false});
        }
        return {ControlProtocolCodec::successResponse(
                    request.id, capabilities(*session)), false};
    }

    return failure(request.id,
                   {QStringLiteral("request.unknown_method"),
                    QStringLiteral("method is not in the v1 registry"),
                    {{QStringLiteral("method"), request.method}}, false});
}

QJsonObject ControlService::capabilities(const ControlSessionState& session)
{
    return {
        {QStringLiteral("sessionId"), session.sessionId},
        {QStringLiteral("version"), 1},
        {QStringLiteral("server"), QJsonObject{
             {QStringLiteral("name"), QStringLiteral("aetherd")},
             {QStringLiteral("version"), QStringLiteral(AETHERSDR_VERSION)}}},
        {QStringLiteral("grants"), QJsonArray{QStringLiteral("observe")}},
        {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("server.read")}},
        {QStringLiteral("limits"), QJsonObject{
             {QStringLiteral("maxMessageBytes"), ProtocolLimits::kMaxMessageBytes}}}
    };
}

std::optional<ProtocolError> ControlService::validateHelloParams(
    const QJsonObject& params)
{
    const QSet<QString> allowedKeys = {
        QStringLiteral("client"), QStringLiteral("versions"), QStringLiteral("auth")};
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (!allowedKeys.contains(it.key())) {
            return ProtocolError{
                QStringLiteral("request.invalid_params"),
                QStringLiteral("unknown hello parameter"),
                {{QStringLiteral("field"), it.key()}}, false};
        }
    }

    const QJsonValue versionsValue = params.value(QStringLiteral("versions"));
    if (!versionsValue.isArray() || versionsValue.toArray().isEmpty()) {
        return ProtocolError{
            QStringLiteral("request.invalid_params"),
            QStringLiteral("versions must be a non-empty array"), {}, false};
    }
    const QJsonArray versions = versionsValue.toArray();
    for (const QJsonValue& value : versions) {
        if (!value.isDouble() || !std::isfinite(value.toDouble())
            || value.toDouble() < 1.0 || std::floor(value.toDouble()) != value.toDouble()) {
            return ProtocolError{
                QStringLiteral("request.invalid_params"),
                QStringLiteral("versions entries must be positive integers"), {}, false};
        }
    }

    const QJsonValue clientValue = params.value(QStringLiteral("client"));
    if (!clientValue.isUndefined()) {
        if (!clientValue.isObject()) {
            return ProtocolError{
                QStringLiteral("request.invalid_params"),
                QStringLiteral("client must be an object"), {}, false};
        }
        const QJsonObject client = clientValue.toObject();
        const QSet<QString> clientKeys = {
            QStringLiteral("name"), QStringLiteral("version")};
        for (auto it = client.constBegin(); it != client.constEnd(); ++it) {
            if (!clientKeys.contains(it.key())) {
                return ProtocolError{
                    QStringLiteral("request.invalid_params"),
                    QStringLiteral("unknown client parameter"),
                    {{QStringLiteral("field"), QStringLiteral("client.%1").arg(it.key())}},
                    false};
            }
        }
        const QString name = client.value(QStringLiteral("name")).toString();
        const QString version = client.value(QStringLiteral("version")).toString();
        if (!client.value(QStringLiteral("name")).isString() || name.isEmpty()
            || name.size() > 128 || !client.value(QStringLiteral("version")).isString()
            || version.isEmpty() || version.size() > 64) {
            return ProtocolError{
                QStringLiteral("request.invalid_params"),
                QStringLiteral("client requires bounded non-empty name and version strings"),
                {}, false};
        }
    }

    if (params.contains(QStringLiteral("auth"))) {
        return ProtocolError{
            QStringLiteral("auth.invalid"),
            QStringLiteral("authentication is not accepted by this local endpoint"),
            {}, false};
    }
    return std::nullopt;
}

ServiceReply ControlService::failure(
    const QString& id, ProtocolError protocolError, bool closeAfterWrite)
{
    return {ControlProtocolCodec::errorResponse(id, protocolError), closeAfterWrite};
}

bool ControlService::acceptsVersionOne(const QJsonObject& params)
{
    const QJsonValue versionsValue = params.value(QStringLiteral("versions"));
    if (!versionsValue.isArray()) {
        return false;
    }
    const QJsonArray versions = versionsValue.toArray();
    for (const QJsonValue& value : versions) {
        if (value.isDouble() && value.toDouble() == 1.0) {
            return true;
        }
    }
    return false;
}

} // namespace AetherSDR::control
