#include "ControlService.h"

#include <QJsonArray>
#include <QSet>
#include <QThread>

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
        QStringLiteral("radioCatalogue"),
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
    if (type == QStringLiteral("server") || type == QStringLiteral("radioCatalogue")) {
        if (!radioSessionValue.isUndefined() || !idValue.isUndefined()) {
            return ProtocolError{QStringLiteral("request.invalid_params"),
                                 QStringLiteral("singleton selector takes only type"), {}, false};
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

DiscoveredRadio catalogueRadio(const QJsonObject& entry)
{
    DiscoveredRadio radio;
    radio.family = entry.value(QStringLiteral("family")).toString();
    radio.serial = entry.value(QStringLiteral("serial")).toString();
    radio.name = entry.value(QStringLiteral("name")).toString();
    radio.model = entry.value(QStringLiteral("model")).toString();
    radio.nickname = entry.value(QStringLiteral("nickname")).toString();
    radio.version = entry.value(QStringLiteral("version")).toString();
    radio.transport = entry.value(QStringLiteral("transport")).toString();
    radio.address = entry.value(QStringLiteral("address")).toString();
    radio.port = static_cast<quint16>(entry.value(QStringLiteral("port")).toInt());
    return radio;
}

} // namespace

ControlService::ControlService(ControlResourceStore* resources,
                               RadioConnectionTarget* connectionTarget)
    : m_resources(resources), m_connectionTarget(connectionTarget)
{
    Q_ASSERT(m_resources);
}

ServiceReply ControlService::handle(
    const QByteArray& bytes, ControlSession* session) const
{
    if (!session || session->thread() != QThread::currentThread()
        || m_resources->thread() != QThread::currentThread()
        || (m_connectionTarget && m_connectionTarget->thread() != QThread::currentThread())) {
        return failure({}, {QStringLiteral("engine.failed"),
                            QStringLiteral("service owning thread required"), {}, false}, true);
    }
    if (session->isRevoked()) {
        return failure({},
                       {QStringLiteral("auth.invalid"),
                        QStringLiteral("session authorization is unavailable"), {}, false},
                       true);
    }
    // Charge every post-handshake frame before parsing or semantic refusal.
    // Otherwise malformed JSON, repeated hello and wrong session IDs bypass
    // the advertised budget. This transport-level error deliberately has no id.
    if (session->isNegotiated() && !session->consumeRequest()) {
        return failure({}, {QStringLiteral("transport.limit_exceeded"),
                            QStringLiteral("request rate exceeded"), {}, false}, true);
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
    if (request.method == QStringLiteral("radio.connect")
        || request.method == QStringLiteral("radio.disconnect")) {
        return handleConnection(request, *session);
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

ServiceReply ControlService::handleConnection(
    const ProtocolRequest& request, const ControlSession& session) const
{
    const auto reject = [&request](const QString& code, const QString& message) {
        return failure(request.id, {code, message, {}, false});
    };
    // Do not let observers probe the target, catalogue, or lifecycle state.
    if (!session.canControl()) {
        return reject(QStringLiteral("auth.grant_denied"), QStringLiteral("control grant required"));
    }
    if (!m_connectionTarget) {
        return reject(QStringLiteral("capability.unavailable"), QStringLiteral("connection control unavailable"));
    }
    const bool connecting = request.method == QStringLiteral("radio.connect");
    const QSet<QString> keys = connecting
        ? QSet<QString>{QStringLiteral("radioSession"), QStringLiteral("radioId"),
                        QStringLiteral("catalogueRevision")}
        : QSet<QString>{QStringLiteral("radioSession")};
    if (const std::optional<ProtocolError> error = onlyKeys(request.params, keys)) {
        return failure(request.id, *error);
    }
    const QJsonValue radioSession = request.params.value(QStringLiteral("radioSession"));
    if (!radioSession.isString() || radioSession.toString().isEmpty()
        || radioSession.toString().size() > ProtocolLimits::kMaxRequestIdChars) {
        return reject(QStringLiteral("request.invalid_params"), QStringLiteral("radioSession must be a bounded identity"));
    }
    // v1 deliberately exposes only the daemon's one engine session. A resource
    // inserted for another session cannot redirect this target.
    if (radioSession.toString() != QStringLiteral("radio-1")
        || !m_resources->get({QStringLiteral("radioSession"), {}, radioSession.toString()})) {
        return reject(QStringLiteral("resource.not_found"), QStringLiteral("radio session unavailable"));
    }
    if (!connecting) {
        m_connectionTarget->disconnectRadio();
        return {ControlProtocolCodec::successResponse(request.id,
                    {{QStringLiteral("accepted"), true}}), false};
    }

    const QJsonValue id = request.params.value(QStringLiteral("radioId"));
    const QJsonValue revision = request.params.value(QStringLiteral("catalogueRevision"));
    const double revisionNumber = revision.toDouble();
    if (!id.isString() || id.toString().size() != 64
        || !revision.isDouble() || !std::isfinite(revisionNumber)
        || revisionNumber < 1 || revisionNumber > 9007199254740991.0
        || std::floor(revisionNumber) != revisionNumber) {
        return reject(QStringLiteral("request.invalid_params"), QStringLiteral("radioId and catalogueRevision are required"));
    }
    const std::optional<ResourceSnapshot> catalogue =
        m_resources->get({QStringLiteral("radioCatalogue"), {}, {}});
    if (!catalogue || !catalogue->value.value(QStringLiteral("running")).toBool()) {
        return reject(QStringLiteral("capability.unavailable"), QStringLiteral("radio catalogue unavailable"));
    }
    if (catalogue->revision != static_cast<quint64>(revisionNumber)) {
        return reject(QStringLiteral("request.conflict"), QStringLiteral("radio catalogue changed; refresh selection"));
    }
    QJsonObject selected;
    const QJsonArray entries = catalogue->value.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("id")) == id) {
            selected = entry;
            break;
        }
    }
    if (selected.isEmpty()) {
        return reject(QStringLiteral("resource.not_found"), QStringLiteral("radio is no longer available"));
    }
    if (selected.value(QStringLiteral("inUse")).toBool()
        || m_connectionTarget->state() != RadioConnectionTarget::State::Idle) {
        return reject(QStringLiteral("request.conflict"), QStringLiteral("radio or engine session is busy"));
    }
    // These fields come exclusively from the bounded catalogue's validated
    // observations. None can be overridden by the requesting client.
    const DiscoveredRadio radio = catalogueRadio(selected);
    if (!m_connectionTarget->supports(radio)) {
        return reject(QStringLiteral("capability.unavailable"), QStringLiteral("radio connection is unsupported"));
    }
    m_connectionTarget->connectRadio(radio);
    return {ControlProtocolCodec::successResponse(request.id,
                {{QStringLiteral("accepted"), true}}), false};
}

QJsonObject ControlService::capabilities(const ControlSession& session) const
{
    const bool observe = session.canObserve();
    QJsonArray grants = observe ? QJsonArray{QStringLiteral("observe")} : QJsonArray{};
    if (session.canControl()) {
        grants.append(QStringLiteral("control"));
    }
    QJsonArray available = observe ? QJsonArray{
        QStringLiteral("server.read"),
        QStringLiteral("radioSession.read"),
        QStringLiteral("slice.read"),
        QStringLiteral("panadapter.read"),
        QStringLiteral("resource.get"),
        QStringLiteral("resource.subscribe"),
        QStringLiteral("resource.unsubscribe")} : QJsonArray{};
    if (observe && m_resources->get({QStringLiteral("radioCatalogue"), {}, {}})) {
        available.append(QStringLiteral("radioCatalogue.read"));
    }
    if (session.canControl() && m_connectionTarget) {
        if (m_connectionTarget->state() == RadioConnectionTarget::State::Idle) {
            const std::optional<ResourceSnapshot> catalogue =
                m_resources->get({QStringLiteral("radioCatalogue"), {}, {}});
            if (catalogue && catalogue->value.value(QStringLiteral("running")).toBool()) {
                const QJsonArray entries = catalogue->value.value(QStringLiteral("entries")).toArray();
                for (const QJsonValue& entry : entries) {
                    if (!entry.toObject().value(QStringLiteral("inUse")).toBool()
                        && m_connectionTarget->supports(catalogueRadio(entry.toObject()))) {
                        available.append(QStringLiteral("radio.connect"));
                        break;
                    }
                }
            }
        } else {
            available.append(QStringLiteral("radio.disconnect"));
        }
    }
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
             {QStringLiteral("requestsPerSecond"), ControlSession::kRequestsPerSecond},
             {QStringLiteral("requestBurst"), ControlSession::kRequestBurst},
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
