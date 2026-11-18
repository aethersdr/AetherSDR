// Remote clients' scoped credentials (aetherd-remote-clients.md): on a network
// session the credential's scope is the grant; local identities never work on
// the network, remote scopes never on the local socket, and a network session
// never reaches transmit. Socket-free: sessions are driven through the service.
#include "core/control/ControlCredentialVault.h"
#include "core/control/ControlService.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}
QString error(const QJsonObject& reply) { return reply.value("error").toObject().value("code").toString(); }
QJsonObject result(const QJsonObject& reply) { return reply.value("result").toObject(); }

class Target final : public TransmitControlTarget {
public:
    qint64 now{100};
    unsigned supported{1};
    int writes{0};
    TxCoordinator::StopRequest pending;
    TxCoordinator coordinator{[this](const auto& operation, auto) {
        pending = coordinator.requestStopConfirmation(operation);
    }, [this] { return now; }};
    TxGrantManager manager{coordinator, [this] { return supported; }};
    TxGrantManager* grants() const override { return const_cast<TxGrantManager*>(&manager); }
    bool ready() const override { return supported && !coordinator.recovering(); }
    bool recovering() const override { return coordinator.recovering(); }
    bool start(const TxGrantManager::Admission& admission) override
    {
        const auto dispatch = admission.operation.beginDispatch(now);
        if (!dispatch) { return false; }
        ++writes;
        return true;
    }
    void emergencyStop() override { coordinator.emergencyStop(); }
    void proveStopped() { check(coordinator.confirmStopped(pending), "injected exact stop proof accepted"); }
};

// Like the production adapter, this target does not own its model/manager.
class TargetView final : public TransmitControlTarget {
public:
    explicit TargetView(Target& target) : m_target(target)
    {
        connect(&target, &TransmitControlTarget::radioInvalidated,
                this, &TransmitControlTarget::radioInvalidated);
    }
    TxGrantManager* grants() const override { return m_target.grants(); }
    bool ready() const override { return m_target.ready(); }
    bool recovering() const override { return m_target.recovering(); }
    bool start(const TxGrantManager::Admission& admission) override { return m_target.start(admission); }
    void emergencyStop() override { m_target.emergencyStop(); }
private:
    Target& m_target;
};

struct Harness {
    Target target;
    ControlResourceStore store;
    ControlCredentials credentials;
    ControlService service{&store};
    ControlCredentials::Record clientRecord{ControlCredentials::generate(ControlCredentials::Role::Client)};
    ControlCredentials::Record adminRecord{ControlCredentials::generate(ControlCredentials::Role::GrantAdmin)};
    QByteArray observeToken, controlToken;
    ControlCredentials::Record observeRecord{
        ControlCredentials::generateRemote(ControlCredentials::Role::RemoteObserve, &observeToken)};
    ControlCredentials::Record controlRecord{
        ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &controlToken)};
    Harness()
    {
        check(credentials.replace({clientRecord, adminRecord, observeRecord, controlRecord})
                  && service.bindCredentials(&credentials) && service.bindTransmitTarget(&target),
              "compose credentials and the TX service before dispatch");
    }
    QJsonObject invoke(ControlSession& session, const QString& method, QJsonObject params = {})
    {
        QJsonObject request{{"v", 1}, {"id", "test"}, {"method", method}, {"params", params}};
        if (method != "hello") { request.insert("sessionId", session.sessionId()); }
        return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
    }
    QJsonObject hello(ControlSession& session, const QByteArray* token)
    {
        QJsonObject p{{"versions", QJsonArray{1}}};
        if (token) { p.insert("auth", QJsonObject{{"scheme", "bearer"}, {"token", QString::fromLatin1(token->toHex())}}); }
        return invoke(session, "hello", p);
    }
};

bool hasMethod(const QJsonObject& caps, const QString& prefix)
{
    for (const QJsonValue& v : caps.value("capabilities").toArray()) {
        if (v.toString().startsWith(prefix)) { return true; }
    }
    return false;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    // ---- records ----
    {
        QByteArray token;
        const auto record = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteControl, &token);
        check(token.size() == ControlCredentials::kSecretBytes && record.secret != token
                  && record.secret == QCryptographicHash::hash(token, QCryptographicHash::Sha256),
              "a remote record keeps the token's digest, never the token");
        check(ControlCredentials::generateRemote(ControlCredentials::Role::Client, &token).id.isEmpty(),
              "generateRemote() refuses a local role");
    }

    // ---- the network: the scope is the grant ----
    {
        Harness h;
        ControlSession anon{&h.store, 65536, SessionAuthorization::NetworkScoped};
        check(error(h.hello(anon, nullptr)) == "auth.required", "a network hello without a credential is refused");
        ControlSession local{&h.store, 65536, SessionAuthorization::NetworkScoped};
        const QByteArray localToken = h.clientRecord.secret;
        check(error(h.hello(local, &localToken)) == "auth.invalid",
              "a local client credential is refused on the network");
        ControlSession observer{&h.store, 65536, SessionAuthorization::NetworkScoped};
        check(error(h.hello(observer, &h.observeToken)).isEmpty() && observer.canObserve() && !observer.canControl(),
              "remote-observe: observe only");
        ControlSession controller{&h.store, 65536, SessionAuthorization::NetworkScoped};
        const QJsonObject caps = result(h.hello(controller, &h.controlToken));
        check(controller.canObserve() && controller.canControl(), "remote-control: observe and control");
        check(!hasMethod(caps, "tx.") && !hasMethod(caps, "txAdmin."),
              "a network session is advertised no transmit method");
        check(error(h.invoke(controller, "tx.status", {{"radioSession", "radio-1"}})) == "auth.grant_denied"
                  && error(h.invoke(controller, "txAdmin.listClients")) == "auth.grant_denied",
              "and every transmit method is refused, with the TX service bound");
        check(controller.principalId() == h.controlRecord.id, "the session carries the paired client's identity");
        // The TX service's own registry: a local client is in it, a network
        // session never is (it is not attached at hello).
        ControlSession admin{&h.store, 65536, SessionAuthorization::Observer};
        const QByteArray adminToken = h.adminRecord.secret;
        ControlSession localClient{&h.store, 65536, SessionAuthorization::Observer};
        const QByteArray clientToken = h.clientRecord.secret;
        check(error(h.hello(admin, &adminToken)).isEmpty() && error(h.hello(localClient, &clientToken)).isEmpty(),
              "a local grant-admin and a local client say hello");
        const QJsonDocument listed(result(h.invoke(admin, "txAdmin.listClients")));
        const QByteArray clients = listed.toJson(QJsonDocument::Compact);
        check(clients.contains(h.clientRecord.id.toLatin1()) && !clients.contains(h.controlRecord.id.toLatin1())
                  && !clients.contains(h.observeRecord.id.toLatin1()),
              "the TX registry lists the local client and no network session");
        check(h.credentials.revoke(h.controlRecord.id) && !controller.isAuthenticated(),
              "revoking the credential ends the network session");
    }

    // ---- the local socket never takes a remote scope ----
    {
        Harness h;
        ControlSession local{&h.store, 65536, SessionAuthorization::Observer};
        check(error(h.hello(local, &h.controlToken)) == "auth.invalid",
              "a remote-scope credential is refused on the local socket");
        ControlSession client{&h.store, 65536, SessionAuthorization::Observer};
        const QByteArray localToken = h.clientRecord.secret;
        check(error(h.hello(client, &localToken)).isEmpty() && client.canObserve(),
              "a local client credential still works on the local socket");
    }

    // ---- the vault: v1 unchanged, v2 only with a remote record ----
    {
        const auto local = ControlCredentials::generate(ControlCredentials::Role::GrantAdmin);
        const auto v1 = ControlCredentialVault::encode({local});
        check(v1 && v1->startsWith("AETHTX01"), "a vault with no remote record is still written as v1");
        QByteArray token;
        const auto remote = ControlCredentials::generateRemote(ControlCredentials::Role::RemoteObserve, &token);
        const auto v2 = ControlCredentialVault::encode({local, remote});
        check(v2 && v2->startsWith("AETHTX02"), "a remote record makes it v2");
        const auto back = v2 ? ControlCredentialVault::decode(*v2) : std::nullopt;
        check(back && back->size() == 2 && back->at(1).role == ControlCredentials::Role::RemoteObserve
                  && back->at(1).secret == remote.secret, "v2 round-trips the remote scope and digest");
        QByteArray forged = *v1;
        forged[8 + 32 + 1] = 3;   // a remote role byte inside a v1 vault
        check(!ControlCredentialVault::decode(forged), "a v1 vault cannot carry a remote role");
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
