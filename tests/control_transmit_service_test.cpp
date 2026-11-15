#include "core/control/ControlService.h"

#include <QCoreApplication>
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
    std::unique_ptr<TargetView> view{std::make_unique<TargetView>(target)};
    ControlResourceStore store;
    ControlCredentials credentials;
    ControlService service{&store};
    ControlCredentials::Record adminRecord{ControlCredentials::generate(ControlCredentials::Role::GrantAdmin)};
    ControlCredentials::Record clientRecord{ControlCredentials::generate(ControlCredentials::Role::Client)};
    ControlSession admin{&store, 65536, SessionAuthorization::Observer};
    ControlSession a{&store, 65536, SessionAuthorization::Observer};
    ControlSession b{&store, 65536, SessionAuthorization::Observer};
    ControlSession observer{&store, 65536, SessionAuthorization::ObserverController};
    Harness()
    {
        check(credentials.replace({adminRecord, clientRecord}) && service.bindCredentials(&credentials)
              && service.bindTransmitTarget(view.get()), "compose credential and TX service before dispatch");
        hello(admin, &adminRecord); hello(a, &clientRecord); hello(b, &clientRecord); hello(observer, nullptr);
    }
    QJsonObject invoke(ControlSession& session, const QString& method, QJsonObject params = {})
    {
        QJsonObject request{{"v", 1}, {"id", "test"}, {"method", method}, {"params", params}};
        if (method != "hello") { request.insert("sessionId", session.sessionId()); }
        return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
    }
    void hello(ControlSession& session, const ControlCredentials::Record* record)
    {
        QJsonObject p{{"versions", QJsonArray{1}}};
        if (record) { p.insert("auth", QJsonObject{{"scheme", "bearer"}, {"token", QString::fromLatin1(record->secret.toHex())}}); }
        check(error(invoke(session, "hello", p)).isEmpty(), "hello authenticates without arming");
    }
    QJsonObject status(ControlSession& session) { return result(invoke(session, "tx.status", {{"radioSession", "radio-1"}})); }
    QJsonObject grant(ControlSession& session)
    {
        const QJsonObject s = status(session);
        return invoke(admin, "txAdmin.issueGrant", {{"radioSession", "radio-1"}, {"radioGeneration", s.value("radioGeneration")},
            {"clientId", s.value("clientId")}, {"activities", QJsonArray{"mox"}},
            {"maximumOperationMs", 1000}, {"lifetimeMs", 10000}, {"keepAliveMs", 5000}});
    }
    QJsonObject params(ControlSession& session)
    {
        const auto s = status(session);
        return {{"radioSession", "radio-1"}, {"radioGeneration", s.value("radioGeneration")}, {"grantId", s.value("grantId")}};
    }
    QJsonObject acquire(ControlSession& session, const QString& serial)
    {
        auto p = params(session); p.insert("intentSerial", serial);
        return invoke(session, "tx.acquire", p);
    }
    QJsonObject keyParams(ControlSession& session, bool key)
    {
        auto p = params(session); p.insert("operationId", status(session).value("operationId")); p.insert("key", key);
        return p;
    }
};

void ownershipAndReplay()
{
    Harness h;
    check(h.target.manager.grantCount() == 0 && h.target.writes == 0, "authentication and local control never arm TX");
    check(error(h.invoke(h.observer, "txAdmin.listClients")) == "auth.grant_denied", "local controller cannot administer grants");
    check(error(h.invoke(h.a, "txAdmin.listClients")) == "auth.grant_denied", "client credential cannot administer grants");
    check(error(h.grant(h.a)).isEmpty() && error(h.grant(h.b)).isEmpty(), "operator explicitly arms two distinct connection lifetimes");
    check(error(h.acquire(h.a, "1")).isEmpty() && h.target.writes == 0, "acquisition owns TX without keying");
    check(error(h.acquire(h.b, "1")) == "tx.busy", "second client cannot share the first client's operation");
    auto p = h.keyParams(h.a, true);
    check(error(h.invoke(h.b, "tx.setKeying", p)) == "auth.grant_denied", "another session cannot replay the owner's IDs");
    check(error(h.invoke(h.a, "tx.setKeying", p)).isEmpty() && h.target.writes == 1, "fresh owner key-on reaches injected engine writer");
    check(error(h.invoke(h.a, "tx.setKeying", p)) == "tx.replay" && h.target.writes == 1, "repeated true edge cannot create another TX dispatch");
    p.insert("key", false);
    check(error(h.invoke(h.a, "tx.setKeying", p)).isEmpty() && h.target.recovering(), "owner off fences before stop proof");
    check(error(h.acquire(h.b, "2")) == "tx.recovering", "other client stays blocked until matching proof");
    h.target.proveStopped();
    check(error(h.acquire(h.b, "2")) == "tx.replay", "refused serial is consumed; no automatic delayed key-on");
    check(error(h.acquire(h.b, "3")).isEmpty(), "fresh intent can acquire after qualified completion");
    h.a.endAuthorization();
    check(!h.target.recovering() && h.target.manager.clientCount() == 1, "disconnecting non-owner leaves current owner untouched");
    h.b.endAuthorization();
    check(h.target.recovering() && h.target.manager.grantCount() == 0, "owner terminal retirement synchronously cancels only its grant");
}

void deadlinesAndGeneration()
{
    Harness h;
    check(error(h.grant(h.a)).isEmpty() && error(h.acquire(h.a, "1")).isEmpty(), "deadline fixture armed and acquired");
    const auto old = h.keyParams(h.a, true);
    h.target.now += 1000;
    check(error(h.invoke(h.a, "tx.setKeying", old)) == "tx.refused" && h.target.writes == 0,
          "expired operation cannot key even before timer delivery");
    h.target.proveStopped();
    const auto keep = h.params(h.a);
    check(error(h.invoke(h.a, "tx.keepAlive", keep)).isEmpty(), "keepalive refreshes client liveness only");
    emit h.target.radioInvalidated();
    check(!h.status(h.a).value("grantLive").toBool()
          && error(h.invoke(h.a, "tx.setKeying", old)) == "request.conflict", "radio generation change retires grant and old intent");
    h.target.supported = 0;
    check(error(h.grant(h.a)) == "capability.unavailable", "unqualified backend cannot issue any transmit grant");
}

void credentialRevocationAndEmergency()
{
    Harness h;
    check(error(h.grant(h.a)).isEmpty() && error(h.acquire(h.a, "1")).isEmpty(), "revocation fixture armed");
    const auto before = h.status(h.a);
    check(error(h.invoke(h.admin, "txAdmin.emergencyStop", {{"radioSession", "radio-1"},
          {"radioGeneration", before.value("radioGeneration")}})).isEmpty()
          && h.target.manager.grantCount() == 0 && h.target.recovering(), "explicit emergency stop retires all grants before global stop");
    h.target.proveStopped();
    check(error(h.grant(h.a)).isEmpty(), "only explicit new grant can rearm after emergency");
    check(h.credentials.revoke(h.clientRecord.id) && h.target.manager.clientCount() == 0
          && h.target.manager.grantCount() == 0 && h.a.isRevoked() && h.b.isRevoked(), "credential retirement closes every bound connection immediately");
}

void targetRetirement()
{
    Harness h;
    check(error(h.grant(h.a)).isEmpty() && error(h.acquire(h.a, "1")).isEmpty(), "adapter retirement fixture owns a live operation");
    h.view.reset();
    check(h.target.manager.grantCount() == 0 && h.target.recovering(),
          "destroying only the adapter retires grants on the surviving model immediately");
    check(error(h.invoke(h.a, "tx.status", {{"radioSession", "radio-1"}})) == "capability.unavailable",
          "retired adapter cannot advertise usable transmit state");
}

void clientCapacityPreservesNonTxSession()
{
    for (const bool engineClients : {false, true}) {
        Harness h;
        std::vector<std::unique_ptr<ControlSession>> clients;
        std::vector<TxGrantManager::Client> directClients;
        for (int i = h.target.manager.clientCount(); i < TxGrantManager::kMaximumClients; ++i) {
            if (engineClients) {
                directClients.push_back(h.target.manager.registerClient(QStringLiteral("trusted-engine")));
            } else {
                clients.push_back(std::make_unique<ControlSession>(&h.store, 65536, SessionAuthorization::Observer));
                h.hello(*clients.back(), &h.clientRecord);
            }
        }
        check(h.target.manager.clientCount() == TxGrantManager::kMaximumClients,
              "capacity fixture fills all optional TX registrations");
        ControlSession excess(&h.store, 65536, SessionAuthorization::ObserverController);
        h.hello(excess, &h.clientRecord);
        const auto capabilities = result(h.invoke(excess, "capabilities.get"));
        check(excess.isNegotiated() && !excess.isRevoked() && excess.canObserve() && excess.canControl()
              && capabilities.value("grants").toArray().contains("observe")
              && capabilities.value("grants").toArray().contains("control")
              && !capabilities.value("capabilities").toArray().contains("tx.status"),
              "TX capacity preserves negotiation and existing observe/control without TX methods");
        check(h.store.upsert({QStringLiteral("server"), {}, {}}, {{"name", "test"}}), "publish observable resource");
        const auto observed = h.invoke(excess, "resource.get", {{"resource", QJsonObject{{"type", "server"}}}});
        check(error(observed).isEmpty() && result(observed).value("value").toObject().value("name") == "test",
              "excess TX client can still observe production resources");
        check(error(h.invoke(excess, "tx.status", {{"radioSession", "radio-1"}})) == "auth.grant_denied"
              && h.target.manager.clientCount() == TxGrantManager::kMaximumClients
              && h.target.manager.grantCount() == 0 && h.target.writes == 0,
              "unregistered connection gains no client ID, grant or keying authority");
        const auto adminList = h.invoke(h.admin, "txAdmin.listClients");
        check(error(adminList).isEmpty(), "administrator remains usable at TX client capacity");
        const auto generation = result(adminList).value("radioGeneration");
        for (const QString& method : {QStringLiteral("tx.acquire"), QStringLiteral("tx.setKeying"),
                                     QStringLiteral("tx.release"), QStringLiteral("tx.cancel"),
                                     QStringLiteral("tx.keepAlive")}) {
            check(error(h.invoke(excess, method, {{"radioSession", "radio-1"},
                  {"radioGeneration", generation}, {"grantId", QString(32, u'a')}})) == "auth.grant_denied",
                  "overflow session cannot bypass TX registration by invoking an unadvertised method");
        }
        h.a.endAuthorization();
        check(!result(h.invoke(excess, "capabilities.get")).value("capabilities").toArray().contains("tx.status"),
              "a freed slot does not silently register an already-negotiated connection");
        ControlSession replacement(&h.store, 65536, SessionAuthorization::Observer);
        h.hello(replacement, &h.clientRecord);
        check(error(h.invoke(replacement, "tx.status", {{"radioSession", "radio-1"}})).isEmpty()
              && h.target.manager.clientCount() == TxGrantManager::kMaximumClients,
              "fresh hello can use a released TX registration slot");
        check(h.credentials.revoke(h.clientRecord.id) && excess.isRevoked() && replacement.isRevoked(),
              "credential retirement also revokes overflow sessions without TX registration");
    }
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ownershipAndReplay(); deadlinesAndGeneration(); credentialRevocationAndEmergency(); targetRetirement();
    clientCapacityPreservesNonTxSession();
    return failures ? 1 : 0;
}
