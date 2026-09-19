#include "TransmitControlService.h"

#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <cmath>

namespace AetherSDR::control {
namespace {

QString freshId() { return QUuid::createUuid().toString(QUuid::Id128); }

bool keys(const QJsonObject& object, const QSet<QString>& expected)
{
    if (object.size() != expected.size()) {
        return false;
    }
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!expected.contains(it.key())) {
            return false;
        }
    }
    return true;
}

bool identifier(const QJsonValue& value)
{
    if (!value.isString() || value.toString().size() != 32) {
        return false;
    }
    for (const QChar ch : value.toString()) {
        if (!(ch >= u'0' && ch <= u'9') && !(ch >= u'a' && ch <= u'f')) {
            return false;
        }
    }
    return true;
}

bool duration(const QJsonValue& value, qint64 maximum)
{
    const double n = value.toDouble();
    return value.isDouble() && std::isfinite(n) && n >= 1 && n <= maximum && std::floor(n) == n;
}

} // namespace

TransmitControlService::TransmitControlService(TransmitControlTarget* target)
    : m_target(target), m_manager(target->grants()), m_generation(freshId())
{
    connect(target, &TransmitControlTarget::radioInvalidated, this, [this] { invalidateRadio(); });
    // The model/grant manager may outlive its protocol adapter. Losing that
    // adapter must fence every outstanding grant before a new composition.
    connect(target, &QObject::destroyed, this, [this] { invalidateRadio(); });
}

TransmitControlService::~TransmitControlService()
{
    while (!m_clients.isEmpty()) {
        retire(m_clients.firstKey());
    }
}

bool TransmitControlService::attach(ControlSession* session)
{
    if (thread() != QThread::currentThread() || !session || session->thread() != thread()
        || session->isRevoked() || !session->isNegotiated() || !m_manager) {
        return false;
    }
    if (session->principalId().isEmpty() || session->canAdministerGrants()) {
        return true; // observers/admins never become implicit TX clients
    }
    if (m_clients.contains(session->sessionId())) {
        return true;
    }
    if (m_manager->clientCount() >= TxGrantManager::kMaximumClients) {
        // TX registration is optional: capacity must not revoke the session's
        // existing observe/control grants. Unregistered sessions get no tx.*
        // methods; a later free slot requires a fresh connection/hello.
        return true;
    }
    auto client = std::make_shared<Client>();
    client->id = freshId();
    client->session = session;
    client->client = m_manager->registerClient(session->principalId(), session->credentialFence());
    if (!m_manager->isClient(client->client)) {
        return false;
    }
    const QString id = session->sessionId();
    m_clients.insert(id, client);
    if (!session->bindAuthorityLifetime(this, [this, id] { retire(id); })) {
        retire(id);
        return false;
    }
    return m_clients.contains(id);
}

void TransmitControlService::retire(const QString& sessionId)
{
    const std::shared_ptr<Client> client = m_clients.take(sessionId);
    if (client && m_manager) {
        m_manager->disconnectClient(client->client);
    }
}

void TransmitControlService::invalidateRadio()
{
    m_generation = freshId();
    if (m_manager) {
        m_manager->invalidateRadio();
    }
    for (const auto& client : std::as_const(m_clients)) {
        client->grant = {};
        client->grantId.clear();
        client->admission = {};
        client->operationId.clear();
        client->started = false;
        client->ended = true;
    }
}

QJsonObject TransmitControlService::status(const Client& client) const
{
    const bool live = m_manager && m_manager->isLive(client.client, client.grant);
    return {{QStringLiteral("clientId"), client.id},
            {QStringLiteral("radioSession"), QStringLiteral("radio-1")},
            {QStringLiteral("radioGeneration"), m_generation},
            {QStringLiteral("grantId"), live ? client.grantId : QString{}},
            {QStringLiteral("operationId"), client.operationId},
            {QStringLiteral("grantLive"), live},
            {QStringLiteral("intentActive"), !client.ended && client.admission.input.valid()},
            {QStringLiteral("ready"), m_target && m_target->ready()},
            {QStringLiteral("recovering"), !m_target || m_target->recovering()}};
}

QJsonArray TransmitControlService::methods(const ControlSession& session) const
{
    if (!m_target || !m_manager || session.isRevoked() || session.principalId().isEmpty()) {
        return {};
    }
    if (session.canAdministerGrants()) {
        return {QStringLiteral("txAdmin.listClients"), QStringLiteral("txAdmin.issueGrant"),
                QStringLiteral("txAdmin.revokeGrant"), QStringLiteral("txAdmin.emergencyStop")};
    }
    if (!m_clients.contains(session.sessionId())) {
        return {};
    }
    QJsonArray result{QStringLiteral("tx.status")};
    const Client& client = *m_clients.value(session.sessionId());
    if (m_manager->isLive(client.client, client.grant)) {
        result.append(QStringLiteral("tx.acquire"));
        result.append(QStringLiteral("tx.setKeying"));
        result.append(QStringLiteral("tx.release"));
        result.append(QStringLiteral("tx.cancel"));
        result.append(QStringLiteral("tx.keepAlive"));
    }
    return result;
}

QJsonObject TransmitControlService::handle(const ProtocolRequest& request, ControlSession* session)
{
    const auto reject = [&request](const char* code, const char* message) {
        return ControlProtocolCodec::errorResponse(request.id,
            {QString::fromLatin1(code), QString::fromLatin1(message), {}, false});
    };
    const auto success = [&request](QJsonObject result = {{QStringLiteral("accepted"), true}}) {
        return ControlProtocolCodec::successResponse(request.id, result);
    };
    if (thread() != QThread::currentThread() || !session || session->thread() != thread()
        || session->isRevoked() || session->principalId().isEmpty()) {
        return reject("auth.grant_denied", "verified credential required");
    }
    if (m_handling || !m_target || !m_manager) {
        return reject("capability.unavailable", "transmit service unavailable");
    }
    m_handling = true;
    const auto finish = qScopeGuard([this] { m_handling = false; });
    const bool admin = request.method.startsWith(QLatin1String("txAdmin."));
    if (admin != session->canAdministerGrants()) {
        return reject("auth.grant_denied", "separate grant-admin or client credential required");
    }
    m_manager->processDeadlines();
    if (session->isRevoked()) {
        return reject("auth.grant_denied", "credential retired");
    }
    const QJsonObject& p = request.params;
    const QString& method = request.method;
    if (admin && method == QLatin1String("txAdmin.listClients")) {
        if (!p.isEmpty()) {
            return reject("request.invalid_params", "listClients takes no parameters");
        }
        QJsonArray clients;
        for (const auto& client : std::as_const(m_clients)) {
            if (client->session && !client->session->isRevoked()) {
                QJsonObject entry = status(*client);
                entry.insert(QStringLiteral("principalId"), client->session->principalId());
                clients.append(entry);
            }
        }
        return success({{QStringLiteral("clients"), clients},
                        {QStringLiteral("radioGeneration"), m_generation}});
    }
    const QSet<QString> radioKeys{QStringLiteral("radioSession"), QStringLiteral("radioGeneration")};
    if (!p.value(QStringLiteral("radioSession")).isString()
        || p.value(QStringLiteral("radioSession")).toString() != QLatin1String("radio-1")) {
        return reject("request.invalid_params", "radioSession must be radio-1");
    }
    // Status bootstraps the current generation, but conveys no authority.
    if (!admin && method == QLatin1String("tx.status")) {
        if (!keys(p, {QStringLiteral("radioSession")})) {
            return reject("request.invalid_params", "status requires radioSession only");
        }
        const auto client = m_clients.value(session->sessionId());
        return client ? success(status(*client)) : reject("auth.grant_denied", "client not registered");
    }
    if (!identifier(p.value(QStringLiteral("radioGeneration")))) {
        return reject("request.invalid_params", "canonical radioGeneration required");
    }
    if (p.value(QStringLiteral("radioGeneration")).toString() != m_generation) {
        return reject("request.conflict", "radio generation changed; explicit reauthorization required");
    }
    if (admin && method == QLatin1String("txAdmin.emergencyStop")) {
        if (!keys(p, radioKeys)) {
            return reject("request.invalid_params", "unexpected emergencyStop parameters");
        }
        // Retire ALL grants before global stop callbacks can re-enter; a fresh
        // grant is required even for previously idle clients after emergency.
        invalidateRadio();
        m_target->emergencyStop();
        return success();
    }
    if (admin) {
        QSet<QString> expected = radioKeys;
        const bool issue = method == QLatin1String("txAdmin.issueGrant");
        if (issue) {
            expected.unite({QStringLiteral("clientId"), QStringLiteral("maximumOperationMs"),
                QStringLiteral("lifetimeMs"), QStringLiteral("keepAliveMs"), QStringLiteral("activities")});
        } else if (method == QLatin1String("txAdmin.revokeGrant")) {
            expected.insert(QStringLiteral("grantId"));
        } else {
            return reject("request.method_not_found", "unknown transmit administration method");
        }
        if (!keys(p, expected) || !identifier(p.value(issue ? QStringLiteral("clientId") : QStringLiteral("grantId")))) {
            return reject("request.invalid_params", "invalid transmit administration parameters");
        }
        std::shared_ptr<Client> selected;
        for (const auto& client : std::as_const(m_clients)) {
            if ((issue ? client->id : client->grantId)
                == p.value(issue ? QStringLiteral("clientId") : QStringLiteral("grantId")).toString()) {
                selected = client;
                break;
            }
        }
        if (!selected || !selected->session || selected->session->isRevoked()) {
            return reject("resource.not_found", "live client or grant not found");
        }
        if (!issue) {
            const bool revoked = m_manager->revoke(selected->grant);
            selected->grantId.clear();
            selected->ended = true;
            return revoked ? success() : reject("auth.grant_denied", "grant already retired");
        }
        if (!duration(p.value(QStringLiteral("maximumOperationMs")), TxGrantManager::kMaximumOperationMs)
            || !duration(p.value(QStringLiteral("lifetimeMs")), TxGrantManager::kMaximumGrantMs)
            || !duration(p.value(QStringLiteral("keepAliveMs")), TxGrantManager::kMaximumKeepAliveMs)
            || p.value(QStringLiteral("activities")) != QJsonValue(QJsonArray{QStringLiteral("mox")})) {
            return reject("request.invalid_params", "explicit bounded policy and activities [mox] required");
        }
        const TxGrantManager::Policy policy{
            p.value(QStringLiteral("maximumOperationMs")).toInteger(),
            p.value(QStringLiteral("lifetimeMs")).toInteger(),
            p.value(QStringLiteral("keepAliveMs")).toInteger(),
            static_cast<unsigned>(TxCoordinator::Activity::Mox)};
        const TxGrantManager::Issuance issuance = m_manager->issue(selected->client, policy);
        if (!issuance.accepted()) {
            return reject(issuance.error == TxGrantManager::Error::Unsupported
                ? "capability.unavailable" : "request.conflict", "grant issuance refused");
        }
        selected->grant = issuance.grant;
        selected->grantId = freshId();
        selected->admission = {};
        selected->operationId.clear();
        selected->started = false;
        selected->ended = false;
        return success(status(*selected));
    }
    const auto client = m_clients.value(session->sessionId());
    if (!client || !identifier(p.value(QStringLiteral("grantId")))
        || client->grantId != p.value(QStringLiteral("grantId")).toString()
        || !m_manager->isLive(client->client, client->grant)) {
        return reject("auth.grant_denied", "this connection has no matching live grant");
    }
    QSet<QString> expected = radioKeys;
    expected.insert(QStringLiteral("grantId"));
    if (method == QLatin1String("tx.keepAlive")) {
        if (!keys(p, expected)) {
            return reject("request.invalid_params", "unexpected keepAlive parameters");
        }
        return m_manager->keepAlive(client->client, client->grant) ? success()
            : reject("auth.grant_denied", "grant expired");
    }
    if (method == QLatin1String("tx.acquire")) {
        expected.insert(QStringLiteral("intentSerial"));
        const QString serialText = p.value(QStringLiteral("intentSerial")).toString();
        bool valid = false;
        const quint64 serial = serialText.toULongLong(&valid, 10);
        if (!keys(p, expected) || !p.value(QStringLiteral("intentSerial")).isString()
            || serialText.size() > 20 || !valid || serial == 0 || QString::number(serial) != serialText) {
            return reject("request.invalid_params", "canonical positive decimal intentSerial required");
        }
        // Admission consumes the serial even on Busy/Recovering. No queue or
        // remembered desire is retried when the current owner releases.
        const TxGrantManager::Admission admission = m_manager->acquire(
            client->client, client->grant, serial, TxCoordinator::Activity::Mox);
        if (!admission.accepted()) {
            if (admission.refusal == TxCoordinator::Refusal::Busy) {
                return reject("tx.busy", "another actor owns transmit");
            }
            if (admission.refusal == TxCoordinator::Refusal::Recovering) {
                return reject("tx.recovering", "stop proof or entered writer still pending");
            }
            return reject(admission.error == TxGrantManager::Error::Replay ? "tx.replay" : "request.conflict",
                          "fresh transmit acquisition refused");
        }
        client->admission = admission;
        client->operationId = freshId();
        client->started = false;
        client->ended = false;
        return success(status(*client));
    }
    expected.insert(QStringLiteral("operationId"));
    const bool keying = method == QLatin1String("tx.setKeying");
    if (keying) {
        expected.insert(QStringLiteral("key"));
    } else if (method != QLatin1String("tx.release") && method != QLatin1String("tx.cancel")) {
        return reject("request.method_not_found", "unknown transmit method");
    }
    if (!keys(p, expected) || !identifier(p.value(QStringLiteral("operationId")))
        || (keying && !p.value(QStringLiteral("key")).isBool())) {
        return reject("request.invalid_params", "canonical operationId and typed key required");
    }
    if (client->operationId != p.value(QStringLiteral("operationId")).toString() || client->ended) {
        return reject("auth.grant_denied", "operation is not owned by this live input");
    }
    if (keying && p.value(QStringLiteral("key")).toBool()) {
        if (client->started) {
            return reject("tx.replay", "key-on was already consumed");
        }
        client->started = true;
        if (!m_target->start(client->admission)) {
            client->ended = true;
            (void)m_manager->cancel(client->client, client->grant, client->admission.operation);
            return reject("tx.refused", "engine transmit preflight refused; fresh acquisition required");
        }
        return success();
    }
    client->ended = true;
    (void)m_manager->release(client->client, client->grant, client->admission.operation);
    return success();
}

} // namespace AetherSDR::control
