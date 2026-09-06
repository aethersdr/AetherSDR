#include "core/control/ControlService.h"
#include "core/control/RadioCatalogue.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {

int failures = 0;
void check(bool condition, const char* description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}

class Source final : public RadioDiscoverySource {
public:
    QStringList enabledSources() const override { return {QStringLiteral("sim")}; }
    void start() override {}
    void stop() override {}
    void publish(const DiscoveredRadio& radio) { emit radioChanged(radio); }
};

class Target final : public RadioConnectionTarget {
public:
    State current{State::Idle};
    bool supported{true};
    int connects{0};
    int disconnects{0};
    DiscoveredRadio last;
    State state() const override { return current; }
    QString errorCode() const override { return {}; }
    bool supports(const DiscoveredRadio&) const override { return supported; }
    void connectRadio(const DiscoveredRadio& radio) override
    {
        last = radio;
        ++connects;
        current = State::Connecting;
        emit stateChanged();
    }
    void disconnectRadio() override
    {
        if (current != State::Idle) {
            ++disconnects;
            current = State::Disconnecting;
            emit stateChanged();
        }
    }
};

ServiceReply invoke(ControlService& service, ControlSession& session,
                    const QString& method, const QJsonObject& params = {},
                    const QString& sessionId = {})
{
    QJsonObject request{{QStringLiteral("v"), 1}, {QStringLiteral("id"), QStringLiteral("test")},
                        {QStringLiteral("method"), method}, {QStringLiteral("params"), params}};
    if (method != QStringLiteral("hello")) {
        request.insert(QStringLiteral("sessionId"), sessionId.isEmpty() ? session.sessionId() : sessionId);
    }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session);
}

QJsonObject hello(ControlService& service, ControlSession& session)
{
    return invoke(service, session, QStringLiteral("hello"),
                  {{QStringLiteral("versions"), QJsonArray{1}}}).message;
}

QString error(const ServiceReply& reply)
{
    return reply.message.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}

struct Fixture {
    ControlResourceStore store;
    Target target;
    ControlService service{&store, &target};
    std::unique_ptr<Source> ownedSource{std::make_unique<Source>()};
    Source* source{ownedSource.get()};
    RadioCatalogue catalogue{std::move(ownedSource), &store};
    ControlSession controller{&store, 4096, SessionAuthorization::ObserverController};
    DiscoveredRadio radio;

    Fixture()
    {
        store.upsert({QStringLiteral("radioSession"), {}, QStringLiteral("radio-1")},
                     {{QStringLiteral("connected"), false}});
        catalogue.start();
        radio.family = QStringLiteral("sim");
        radio.serial = QStringLiteral("DEMO-0001");
        radio.transport = QStringLiteral("sim");
        radio.nickname = QStringLiteral("selected nickname");
        source->publish(radio);
        hello(service, controller);
    }

    QJsonObject params() const
    {
        const ResourceSnapshot snapshot = *store.get({QStringLiteral("radioCatalogue"), {}, {}});
        return {{QStringLiteral("radioSession"), QStringLiteral("radio-1")},
                {QStringLiteral("catalogueRevision"), static_cast<qint64>(snapshot.revision)},
                {QStringLiteral("radioId"), snapshot.value.value(QStringLiteral("entries")).toArray()
                    .first().toObject().value(QStringLiteral("id"))}};
    }
};

void authorization()
{
    Fixture f;
    for (SessionAuthorization authorization : {SessionAuthorization::Unauthenticated,
             SessionAuthorization::AuthenticatedWithoutGrants, SessionAuthorization::Observer,
             SessionAuthorization::Controller, SessionAuthorization::ObserverController}) {
        ControlSession session(&f.store, 4096, authorization);
        check(!session.canControl(), "control cannot precede negotiation");
        const QJsonObject welcome = hello(f.service, session);
        const bool permitted = authorization == SessionAuthorization::Controller
            || authorization == SessionAuthorization::ObserverController;
        const QJsonArray grants = welcome.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("grants")).toArray();
        check(grants.contains(QStringLiteral("control")) == permitted, "only trusted control contexts advertise control");
        check(!grants.contains(QStringLiteral("transmit")), "transmit grant must remain absent");
        const ServiceReply connectReply = invoke(f.service, session, QStringLiteral("radio.connect"), f.params());
        if (permitted) {
            check(error(connectReply).isEmpty(), "authorized connect accepted");
            f.target.current = RadioConnectionTarget::State::Idle;
        } else {
            check(!error(connectReply).isEmpty(), "observer/unauthenticated/no-grants connect denied");
        }
        if (authorization == SessionAuthorization::Controller) {
            check(!session.canObserve(), "control must not imply observation");
            check(error(invoke(f.service, session, QStringLiteral("resource.get")))
                      == QStringLiteral("auth.grant_denied"), "control-only cannot read resources");
        }
    }
    check(f.target.connects == 2, "only two explicitly authorized intents reach the target");

    ControlSession observer(&f.store, 4096, SessionAuthorization::Observer);
    hello(f.service, observer);
    for (const QString& method : {QStringLiteral("radio.connect"), QStringLiteral("radio.disconnect")}) {
        check(error(invoke(f.service, observer, method)) == QStringLiteral("auth.grant_denied"),
              "grant refusal precedes parameter and target lookup");
        check(error(invoke(f.service, f.controller, method, f.params(), observer.sessionId()))
                  == QStringLiteral("session.invalid"), "another client session cannot authorize an intent");
    }
    f.controller.revokeAuthorization();
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), f.params()))
              == QStringLiteral("auth.invalid"), "revocation blocks connect");
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.disconnect")))
              == QStringLiteral("auth.invalid"), "revocation blocks disconnect");
    check(f.target.connects == 2 && f.target.disconnects == 0, "denials leave the injected transport untouched");

    ControlSession claimed(&f.store, 4096, SessionAuthorization::Observer);
    check(error(invoke(f.service, claimed, QStringLiteral("hello"),
          {{QStringLiteral("versions"), QJsonArray{1}}, {QStringLiteral("grants"), QJsonArray{QStringLiteral("control")}}}))
              == QStringLiteral("request.invalid_params"), "hello cannot claim control");
}

void validationAndLifecycle()
{
    Fixture f;
    const QJsonObject valid = f.params();
    for (const QString& key : {QStringLiteral("host"), QStringLiteral("port"), QStringLiteral("auth"),
                               QStringLiteral("family"), QStringLiteral("command"), QStringLiteral("force")}) {
        QJsonObject invalid = valid;
        invalid.insert(key, QStringLiteral("not accepted"));
        check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), invalid))
                  == QStringLiteral("request.invalid_params"), "endpoint/credential/force overrides rejected");
    }
    for (const QJsonValue& revision : {QJsonValue(), QJsonValue(true), QJsonValue("1"),
                                      QJsonValue(0), QJsonValue(-1), QJsonValue(1.5), QJsonValue(1e30)}) {
        QJsonObject invalid = valid;
        invalid.insert(QStringLiteral("catalogueRevision"), revision);
        check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), invalid))
                  == QStringLiteral("request.invalid_params"), "invalid revision rejected");
    }
    QJsonObject other = valid;
    other.insert(QStringLiteral("radioSession"), QStringLiteral("radio-2"));
    f.store.upsert({QStringLiteral("radioSession"), {}, QStringLiteral("radio-2")}, {});
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), other))
              == QStringLiteral("resource.not_found"), "other engine resource cannot redirect the single-session target");
    other = valid;
    other.insert(QStringLiteral("radioId"), QString(64, QLatin1Char('0')));
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), other))
              == QStringLiteral("resource.not_found"), "unknown catalogue identity rejected");
    f.radio.nickname = QStringLiteral("changed");
    f.source->publish(f.radio);
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), valid))
              == QStringLiteral("request.conflict"), "stale selection rejected");
    f.radio.inUse = true;
    f.source->publish(f.radio);
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), f.params()))
              == QStringLiteral("request.conflict"), "busy observation cannot trigger takeover");
    f.radio.inUse = false;
    f.source->publish(f.radio);
    f.target.supported = false;
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), f.params()))
              == QStringLiteral("capability.unavailable"), "unsupported backend rejected");
    f.target.supported = true;
    check(f.target.connects == 0, "all validation failures have zero connection effects");
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), f.params())).isEmpty(),
          "valid intent accepted");
    check(f.target.last.nickname == f.radio.nickname && f.target.last.serial == f.radio.serial,
          "target receives current catalogue values");
    for (RadioConnectionTarget::State state : {RadioConnectionTarget::State::Connecting,
             RadioConnectionTarget::State::Connected, RadioConnectionTarget::State::Disconnecting}) {
        f.target.current = state;
        check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), f.params()))
                  == QStringLiteral("request.conflict"), "active lifecycle cannot be replaced");
    }
    f.target.current = RadioConnectionTarget::State::Connecting;
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.disconnect"),
                {{QStringLiteral("radioSession"), QStringLiteral("radio-1")}})).isEmpty(), "disconnect accepts cancellation");
    check(f.target.disconnects == 1 && f.target.connects == 1, "exactly one connect and cancellation intent");
    check(f.service.capabilities(f.controller).value(QStringLiteral("capabilities")).toArray()
              .contains(QStringLiteral("radio.disconnect")), "disconnect advertised during teardown");
    f.target.current = RadioConnectionTarget::State::Idle;
    f.catalogue.stop();
    check(error(invoke(f.service, f.controller, QStringLiteral("radio.connect"), valid))
              == QStringLiteral("capability.unavailable"), "stopped source cannot connect");
    for (const QString& method : {QStringLiteral("transmit.setMox"), QStringLiteral("tx.acquire"),
                                 QStringLiteral("slice.setFrequency"), QStringLiteral("invoke")}) {
        check(error(invoke(f.service, f.controller, method)) == QStringLiteral("request.unknown_method"),
              "TX and out-of-scope control remain absent");
    }
}

void limitsAndLifetime()
{
    Fixture f;
    ControlService unavailable(&f.store);
    check(error(invoke(unavailable, f.controller, QStringLiteral("radio.connect"), f.params()))
              == QStringLiteral("capability.unavailable"), "absent target cannot dispatch");
    auto target = std::make_unique<Target>();
    ControlService service(&f.store, target.get());
    target.reset();
    check(error(invoke(service, f.controller, QStringLiteral("radio.connect"), f.params()))
              == QStringLiteral("capability.unavailable"), "destroyed target fails closed");
    check(!service.capabilities(f.controller).value(QStringLiteral("capabilities")).toArray()
              .contains(QStringLiteral("radio.connect")), "destroyed target is not advertised");

    // Check the release-build dispatch guard without moving or mutating any
    // engine object from the foreign thread. No socket or backend is involved.
    ServiceReply foreignReply;
    const QJsonObject params = f.params();
    std::unique_ptr<QThread> worker(QThread::create([&] {
        foreignReply = invoke(f.service, f.controller, QStringLiteral("radio.connect"), params);
    }));
    worker->start();
    worker->wait();
    check(error(foreignReply) == QStringLiteral("engine.failed") && foreignReply.closeAfterWrite
              && f.target.connects == 0, "foreign-thread dispatch fails closed before target access");
}

void requestBudget()
{
    Fixture f;
    // Fixed time makes the exact burst independent of slow CI/sanitizer hosts.
    for (int scenario = 0; scenario < 4; ++scenario) {
        qint64 now = 0;
        ControlSession session(&f.store, 4096, SessionAuthorization::ObserverController,
                               nullptr, [&] { return now; });
        hello(f.service, session);
        const auto request = [&] {
            switch (scenario) {
            case 1: return f.service.handle("{", &session);
            case 2: return invoke(f.service, session, QStringLiteral("hello"));
            case 3: return invoke(f.service, session, QStringLiteral("capabilities.get"), {},
                                 QStringLiteral("wrong-session"));
            default: return invoke(f.service, session, QStringLiteral("capabilities.get"));
            }
        };
        for (int i = 0; i < ControlSession::kRequestBurst; ++i) {
            check(error(request()) != QStringLiteral("transport.limit_exceeded"),
                  "the advertised burst is available, including rejected frames");
        }
        const ServiceReply limited = request();
        check(error(limited) == QStringLiteral("transport.limit_exceeded") && limited.closeAfterWrite
                  && !limited.message.contains(QStringLiteral("id")),
              "all post-handshake frame shapes consume the burst and close without an id");
        now += 10'000'000'000LL;
        check(error(invoke(f.service, session, QStringLiteral("radio.connect"), f.params()))
                  == QStringLiteral("transport.limit_exceeded") && f.target.connects == 0,
              "rate exhaustion cannot regain control while transport closure is pending");
        check(error(invoke(f.service, f.controller, QStringLiteral("capabilities.get"))).isEmpty(),
              "another client's rate budget is independent");
    }

    qint64 now = 0;
    ControlSession refilled(&f.store, 4096, SessionAuthorization::Observer,
                            nullptr, [&] { return now; });
    hello(f.service, refilled);
    const auto request = [&] { return invoke(f.service, refilled, QStringLiteral("capabilities.get")); };
    for (int i = 0; i < ControlSession::kRequestBurst; ++i) {
        check(error(request()).isEmpty(), "initial exact burst succeeds");
    }
    now += 10'000'000; // One token at 100/s, before trying an exhausted request.
    check(error(request()).isEmpty(), "monotonic elapsed time replenishes one token");
    now += 10'000'000'000LL; // Idle time cannot bank more than the advertised burst.
    for (int i = 0; i < ControlSession::kRequestBurst; ++i) {
        check(error(request()).isEmpty(), "refill restores the full burst");
    }
    check(error(request()) == QStringLiteral("transport.limit_exceeded"),
          "refill is capped at the advertised burst");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    authorization();
    validationAndLifecycle();
    limitsAndLifetime();
    requestBudget();
    return failures == 0 ? 0 : 1;
}
