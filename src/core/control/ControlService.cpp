#include "ControlService.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace AetherSDR::control {
namespace {

constexpr qsizetype kMaxSelectorsPerSubscription = 64;

std::optional<ProtocolError> onlyKeys(
    const QJsonObject& object, const QSet<QString>& allowed)
{
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            return ProtocolError{QStringLiteral("request.invalid_params"),
                                 QStringLiteral("unknown parameter"),
                                 {{QStringLiteral("field"), it.key()}}, false};
        }
    }
    return std::nullopt;
}

std::optional<ProtocolError> parseSelector(
    const QJsonValue& value, bool wildcardAllowed, ResourceSelector* selector)
{
    if (!selector || !value.isObject()) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("resource selector must be an object"), {}, false};
    }
    const QJsonObject object = value.toObject();
    if (const std::optional<ProtocolError> keyError = onlyKeys(
            object, {QStringLiteral("type"), QStringLiteral("radioSession"),
                     QStringLiteral("id")})) {
        return keyError;
    }

    const QJsonValue typeValue = object.value(QStringLiteral("type"));
    if (!typeValue.isString()) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("resource type must be a string"), {}, false};
    }
    const QString type = typeValue.toString();
    const QSet<QString> supportedTypes{
        QStringLiteral("server"), QStringLiteral("radioSession"),
        QStringLiteral("slice"), QStringLiteral("panadapter")};
    if (!supportedTypes.contains(type)) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("unsupported resource type"),
                             {{QStringLiteral("type"), type}}, false};
    }

    const QJsonValue radioSessionValue = object.value(QStringLiteral("radioSession"));
    const QJsonValue idValue = object.value(QStringLiteral("id"));
    const auto validOptionalId = [](const QJsonValue& field) {
        return field.isUndefined()
            || (field.isString() && !field.toString().isEmpty()
                && field.toString().size() <= ProtocolLimits::kMaxRequestIdChars);
    };
    if (!validOptionalId(radioSessionValue) || !validOptionalId(idValue)) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("resource identifiers must be bounded non-empty strings"),
                             {}, false};
    }

    const QString radioSession = radioSessionValue.toString();
    const QString id = idValue.toString();
    if (type == QStringLiteral("server")) {
        if (!radioSessionValue.isUndefined() || !idValue.isUndefined()) {
            return ProtocolError{QStringLiteral("request.invalid_params"),
                                 QStringLiteral("server selector takes only type"), {}, false};
        }
    } else if (type == QStringLiteral("radioSession")) {
        if (!radioSessionValue.isUndefined() || (!wildcardAllowed && id.isEmpty())) {
            return ProtocolError{QStringLiteral("request.invalid_params"),
                                 QStringLiteral("radioSession selector requires id only"), {}, false};
        }
    } else if (radioSession.isEmpty() || (!wildcardAllowed && id.isEmpty())) {
        return ProtocolError{QStringLiteral("request.invalid_params"),
                             QStringLiteral("slice and panadapter selectors require radioSession and id"),
                             {}, false};
    }

    *selector = ResourceSelector{type, radioSession, id};
    return std::nullopt;
}

ResourceAddress exactAddress(const ResourceSelector& selector)
{
    return {selector.type, selector.radioSession, selector.id};
}

} // namespace

ControlService::ControlService(ControlResourceStore* resources)
    : m_resources(resources)
{
    Q_ASSERT(m_resources);
}

ServiceReply ControlService::handle(
    const QByteArray& bytes, ControlSession* session) const
{
    if (!session || session->isRevoked()) {
        return failure({},
                       {QStringLiteral("auth.invalid"),
                        QStringLiteral("session authorization is unavailable"), {}, false},
                       true);
    }
    const ParseResult parsed = ControlProtocolCodec::parseRequest(bytes);
    if (!parsed.ok()) {
        return failure(parsed.requestId,
                       parsed.error.value_or(ProtocolError{
                           QStringLiteral("protocol.invalid_envelope"),
                           QStringLiteral("request could not be parsed"), {}, false}),
                       !session->isNegotiated());
    }

    const ProtocolRequest& request = *parsed.request;
    if (!session->isNegotiated()) {
        if (!session->isAuthenticated()) {
            return failure(request.id,
                           {QStringLiteral("auth.required"),
                            QStringLiteral("authenticated transport context required"), {}, false},
                           true);
        }
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

        session->completeNegotiation();
        return {ControlProtocolCodec::successResponse(
                    request.id, capabilities(*session)), false};
    }

    if (request.isHello()) {
        return failure(request.id,
                       {QStringLiteral("protocol.invalid_envelope"),
                        QStringLiteral("hello may only be sent once"), {}, false});
    }
    if (request.sessionId != session->sessionId()) {
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
    if (request.method == QStringLiteral("resource.get")) {
        if (const std::optional<ProtocolError> error = session->observationError()) {
            return failure(request.id, *error);
        }
        if (const std::optional<ProtocolError> keyError = onlyKeys(
                request.params, {QStringLiteral("resource")})) {
            return failure(request.id, *keyError);
        }
        ResourceSelector selector;
        if (const std::optional<ProtocolError> selectorError = parseSelector(
                request.params.value(QStringLiteral("resource")), false, &selector)) {
            return failure(request.id, *selectorError);
        }
        const std::optional<ResourceSnapshot> snapshot =
            m_resources->get(exactAddress(selector));
        if (!snapshot) {
            return failure(request.id,
                           {QStringLiteral("resource.not_found"),
                            QStringLiteral("resource does not exist"), {}, false});
        }
        return {ControlProtocolCodec::successResponse(request.id, snapshot->toJson()), false};
    }
    if (request.method == QStringLiteral("resource.subscribe")) {
        if (const std::optional<ProtocolError> error = session->observationError()) {
            return failure(request.id, *error);
        }
        if (const std::optional<ProtocolError> keyError = onlyKeys(
                request.params, {QStringLiteral("resources")})) {
            return failure(request.id, *keyError);
        }
        const QJsonValue resourcesValue = request.params.value(QStringLiteral("resources"));
        if (!resourcesValue.isArray() || resourcesValue.toArray().isEmpty()
            || resourcesValue.toArray().size() > kMaxSelectorsPerSubscription) {
            return failure(request.id,
                           {QStringLiteral("request.invalid_params"),
                            QStringLiteral("resources must contain between 1 and 64 selectors"),
                            {}, false});
        }
        QList<ResourceSelector> selectors;
        const QJsonArray resourceArray = resourcesValue.toArray();
        selectors.reserve(resourceArray.size());
        for (const QJsonValue& value : resourceArray) {
            ResourceSelector selector;
            if (const std::optional<ProtocolError> selectorError =
                    parseSelector(value, true, &selector)) {
                return failure(request.id, *selectorError);
            }
            selectors.append(selector);
        }
        QJsonObject result;
        if (const std::optional<ProtocolError> subscribeError =
                session->subscribe(selectors, &result)) {
            return failure(request.id, *subscribeError);
        }
        return {ControlProtocolCodec::successResponse(request.id, result), false};
    }
    if (request.method == QStringLiteral("resource.unsubscribe")) {
        if (const std::optional<ProtocolError> error = session->observationError()) {
            return failure(request.id, *error);
        }
        if (const std::optional<ProtocolError> keyError = onlyKeys(
                request.params, {QStringLiteral("subscription")})) {
            return failure(request.id, *keyError);
        }
        const QJsonValue subscriptionValue =
            request.params.value(QStringLiteral("subscription"));
        if (!subscriptionValue.isString() || subscriptionValue.toString().isEmpty()
            || subscriptionValue.toString().size() > ProtocolLimits::kMaxRequestIdChars) {
            return failure(request.id,
                           {QStringLiteral("request.invalid_params"),
                            QStringLiteral("subscription must be a bounded non-empty string"),
                            {}, false});
        }
        QJsonObject result;
        if (const std::optional<ProtocolError> unsubscribeError =
                session->unsubscribe(subscriptionValue.toString(), &result)) {
            return failure(request.id, *unsubscribeError);
        }
        return {ControlProtocolCodec::successResponse(request.id, result), false};
    }

    return failure(request.id,
                   {QStringLiteral("request.unknown_method"),
                    QStringLiteral("method is not in the v1 registry"),
                    {{QStringLiteral("method"), request.method}}, false});
}

QJsonObject ControlService::capabilities(const ControlSession& session) const
{
    const bool observe = session.canObserve();
    const QJsonArray grants = observe ? QJsonArray{QStringLiteral("observe")} : QJsonArray{};
    const QJsonArray available = observe ? QJsonArray{
        QStringLiteral("server.read"),
        QStringLiteral("radioSession.read"),
        QStringLiteral("slice.read"),
        QStringLiteral("panadapter.read"),
        QStringLiteral("resource.get"),
        QStringLiteral("resource.subscribe"),
        QStringLiteral("resource.unsubscribe")} : QJsonArray{};
    return {
        {QStringLiteral("sessionId"), session.sessionId()},
        {QStringLiteral("version"), 1},
        {QStringLiteral("server"), QJsonObject{
             {QStringLiteral("name"), QStringLiteral("aetherd")},
             {QStringLiteral("version"), QStringLiteral(AETHERSDR_VERSION)}}},
        {QStringLiteral("grants"), grants},
        {QStringLiteral("capabilities"), available},
        {QStringLiteral("limits"), QJsonObject{
             {QStringLiteral("maxMessageBytes"), ProtocolLimits::kMaxMessageBytes},
             {QStringLiteral("maxSubscriptions"), ControlSession::kMaxSubscriptions},
             {QStringLiteral("maxSelectorsPerSubscription"),
              kMaxSelectorsPerSubscription},
             {QStringLiteral("maxQueuedOutputBytes"),
              session.maxQueuedOutputBytes()}}}
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
