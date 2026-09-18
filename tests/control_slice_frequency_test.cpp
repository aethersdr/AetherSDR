#include "TestSettingsProfile.h"
#include "core/control/ControlService.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <cstdio>
#include <limits>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

// Injected engine transport: no sockets, discovery, DSP, timers or peer.
// It records DOWN intents; tests feed normalized observations separately.
class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int tunes{0};
    int keys{0};
    int lastSlice{-1};
    double lastHz{0};
    mutable int capabilityReads{0};
    RadioCapabilities capabilities() const override { ++capabilityReads; return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int id, double hz) override { ++tunes; lastSlice = id; lastHz = hz; }
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { ++keys; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

class Connection final : public RadioConnectionTarget {
public:
    State current{State::Idle};
    State state() const override { return current; }
    QString errorCode() const override { return {}; }
    bool supports(const DiscoveredRadio&) const override { return true; }
    void connectRadio(const DiscoveredRadio&) override {}
    void disconnectRadio() override { change(State::Disconnecting); }
    void change(State next) { current = next; emit stateChanged(); }
};

QJsonObject call(ControlService& service, ControlSession& session, const QString& method,
                 const QJsonObject& params = {}, const QString& sessionId = {})
{
    QJsonObject request{{"v", 1}, {"id", "request"}, {"method", method}, {"params", params}};
    if (method != QStringLiteral("hello")) {
        request.insert("sessionId", sessionId.isEmpty() ? session.sessionId() : sessionId);
    }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
}

QString error(const QJsonObject& response)
{
    return response.value("error").toObject().value("code").toString();
}

struct Fixture {
    RadioModel radio;
    Connection connection;
    ControlResourceStore store;
    std::unique_ptr<SliceFrequencyTarget> target;
    ControlService service{&store};
    ControlSession controller{&store, 65536, SessionAuthorization::ObserverController};
    RadioResourceAdapter adapter{&radio, &store, QStringLiteral("radio-1"), nullptr, &connection};
    RecordingBackend* backend{nullptr};
    const ResourceAddress address{QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")};

    Fixture()
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        backend->caps.sliceFrequencyControl = {SliceFrequencyControl::Authority::Radio,
                                               100'000, 60'000'000};
        radio.setBackendForTest(std::move(owned), QStringLiteral("frequency-test"));
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")),
              "socket-free model slice fixture installed");
        target = makeModelSliceFrequencyTarget(&radio, &connection);
        check(target && service.bindFrequencyTarget(target.get()), "production target bound before dispatch");
        backend->connected = true;
        connection.change(RadioConnectionTarget::State::Connected);
        report(14'225'000);
        hello(controller);
    }

    void hello(ControlSession& session)
    {
        check(call(service, session, "hello", {{"versions", QJsonArray{1}}}).contains("result"),
              "session negotiated");
    }

    void report(qint64 hz)
    {
        SliceDelta delta;
        delta.frequency = static_cast<double>(hz) / 1'000'000.0;
        radio.slice(0)->applyChanges(delta);
    }

    QJsonObject params(qint64 hz = 14'230'000) const
    {
        const std::optional<ResourceSnapshot> snapshot = store.get(address);
        return {{"radioSession", "radio-1"}, {"slice", "0"},
                {"expectedRevision", static_cast<qint64>(snapshot ? snapshot->revision : 0)}, {"hz", hz}};
    }

    QJsonObject tune(const QJsonObject& params) { return call(service, controller, "slice.setFrequency", params); }
    void rejected(const QJsonObject& params, const char* code)
    {
        const int before = backend->tunes;
        const QString actual = error(tune(params));
        if (actual != QString::fromLatin1(code)) {
            std::printf("Expected %s, got %s\n", code, qPrintable(actual));
        }
        check(actual == QString::fromLatin1(code) && backend->tunes == before,
              "refusal has expected error and dispatches no intent");
    }
};

void authorizationAndSchema()
{
    Fixture f;
    for (SessionAuthorization authorization : {SessionAuthorization::Observer,
                                               SessionAuthorization::AuthenticatedWithoutGrants}) {
        ControlSession denied(&f.store, 65536, authorization);
        f.hello(denied);
        check(error(call(f.service, denied, "slice.setFrequency", {{"invalid", true}}))
                  == "auth.grant_denied" && f.backend->tunes == 0,
              "grant denial precedes target/schema access");
        check(!f.service.capabilities(denied).value("capabilities").toArray()
                   .contains("slice.setFrequency"), "observer never advertises mutation");
    }
    QJsonObject params = f.params();
    params.insert("force", true);
    f.rejected(params, "request.invalid_params");
    for (const char* field : {"radioSession", "slice", "expectedRevision", "hz"}) {
        params = f.params();
        params.remove(field);
        f.rejected(params, "request.invalid_params");
    }
    for (QJsonValue value : {QJsonValue(0), QJsonValue(-1), QJsonValue(1.25),
                            QJsonValue(true), QJsonValue("14225000"),
                            QJsonValue(QJsonValue::Null), QJsonValue(9007199254740992.0)}) {
        params = f.params();
        params.insert("hz", value);
        f.rejected(params, "request.invalid_params");
        params = f.params();
        params.insert("expectedRevision", value);
        f.rejected(params, "request.invalid_params");
    }
    for (const char* id : {"-1", "+0", "00", "0 ", "2147483648", "0\nraw command"}) {
        params = f.params(); params.insert("slice", id);
        f.rejected(params, "request.invalid_params");
    }
    params = f.params(); params.insert("slice", "1");
    f.rejected(params, "resource.not_found");
    params = f.params(); params.insert("radioSession", "radio-2");
    f.rejected(params, "resource.not_found");
    check(error(call(f.service, f.controller, "slice.setFrequency", f.params(), "another-session"))
              == "session.invalid" && f.backend->tunes == 0, "foreign protocol session cannot tune");
    f.controller.revokeAuthorization();
    check(error(f.tune(f.params())) == "auth.invalid" && f.backend->tunes == 0,
          "revocation prevents subsequent tuning");
}

void authoritativeObservations()
{
    Fixture f;
    const quint64 originalRevision = f.store.get(f.address)->revision;
    const QJsonObject request = f.params();
    call(f.service, f.controller, "resource.subscribe",
         {{"resources", QJsonArray{QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}}}}});
    (void) f.controller.takePendingFrames();
    check(f.tune(request).value("result").toObject().value("accepted").toBool()
              && f.backend->tunes == 1 && f.backend->lastSlice == 0
              && f.backend->lastHz == 14'230'000,
          "production target dispatches exactly one typed Hz intent");
    check(f.store.get(f.address)->value.value("frequencyObservation").toObject()
                  .value("authority").toString() == "radio",
          "published observation authority reflects the bound backend");
    check(f.radio.slice(0)->frequency() == 14.225
              && f.store.get(f.address)->revision == originalRevision
              && f.controller.takePendingFrames().isEmpty(),
          "acceptance never optimistically changes the model or observation");

    ControlSession second(&f.store, 65536, SessionAuthorization::ObserverController);
    f.hello(second);
    QJsonObject competing = request; competing.insert("hz", 14'235'000);
    check(call(f.service, second, "slice.setFrequency", competing).contains("result")
              && f.backend->tunes == 2 && f.backend->lastHz == 14'235'000,
          "same observed revision permits ordered pending intents, not compare-and-swap");
    const int readsBeforeReport = f.backend->capabilityReads;
    f.report(14'232'000);
    check(f.backend->capabilityReads == readsBeforeReport,
          "a frequency report republishes without re-reading backend capabilities");
    const ResourceSnapshot changed = *f.store.get(f.address);
    check(changed.revision > originalRevision
              && changed.value.value("frequencyObservation").toObject().value("hz").toInteger() == 14'232'000
              && !f.controller.takePendingFrames().isEmpty(),
          "a differing backend report wins and reaches subscribers");
    f.rejected(request, "request.conflict");
    f.report(14'232'000);
    check(f.store.get(f.address)->revision == changed.revision,
          "unchanged reports do not fabricate resource revisions");
    check(f.tune(f.params(14'232'000)).contains("result"), "same-current-frequency intent succeeds");

    SliceModel* slice = f.radio.slice(0);
    slice->setFrequency(14.240);
    check(slice->frequency() == 14.240 && slice->reportedFrequency() == 14.232,
          "legacy desktop optimistic value remains separate");
    const quint64 optimisticRevision = f.store.get(f.address)->revision;
    f.report(14'240'000);
    check(f.store.get(f.address)->revision > optimisticRevision
              && slice->reportedFrequency() == 14.240,
          "echo equal to optimistic value still updates observed state");
    slice->invalidateFrequencyObservation();
    check(!f.target->available()
              && f.store.get(f.address)->value.value("frequencyObservation").toObject().value("hz").isNull(),
          "unknown observation is explicit and cannot enable tuning");
    f.report(14'240'000);
    check(f.target->available(), "fresh same-value observation restores eligibility");
    check(f.backend->keys == 0, "all tuning and observation paths emit no keying intent");
}

void coverageAndSafety()
{
    Fixture f;
    f.rejected(f.params(99'999), "request.out_of_range");
    f.rejected(f.params(60'000'001), "request.out_of_range");
    check(f.tune(f.params(100'000)).contains("result"), "lower coverage endpoint accepted");
    check(f.tune(f.params(60'000'000)).contains("result"), "upper coverage endpoint accepted");
    f.backend->caps.declaredBandRanges = {{"low", 100'000, 200'000}, {"high", 50'000'000, 60'000'000}};
    f.rejected(f.params(), "request.out_of_range");
    check(f.tune(f.params(50'000'000)).contains("result"), "discontinuous coverage endpoint accepted");
    f.backend->caps.declaredBandRanges.clear();
    f.backend->caps.sliceFrequencyControl.minimumHz = 0;
    check(!f.target->available(), "unknown coverage is not advertised");
    f.rejected(f.params(), "capability.unavailable");
    f.backend->caps.sliceFrequencyControl.minimumHz = 100'000;
    f.backend->caps.sliceFrequencyControl.authority = SliceFrequencyControl::Authority::Unknown;
    f.rejected(f.params(), "capability.unavailable");
    f.backend->caps.sliceFrequencyControl.authority = SliceFrequencyControl::Authority::Radio;
    f.radio.slice(0)->setLocked(true);
    f.rejected(f.params(), "request.conflict");
    f.radio.slice(0)->setLocked(false);
    SliceDelta txSelection;
    txSelection.txSlice = true;
    f.radio.slice(0)->applyChanges(txSelection);
    check(f.radio.slice(0)->isTxSlice(), "TX-slice designation is independent of active transmission");
    f.backend->caps.canTransmit = true;
    check(!f.target->available(), "TX-capable backend starts with unknown idle state");
    f.rejected(f.params(), "request.conflict");
    f.radio.radioTransmittingChanged(false);
    f.rejected(f.params(), "request.conflict");
    f.radio.radioTransmitConfirmed(false);
    check(f.target->available() && f.tune(f.params()).contains("result"),
          "explicit idle readback admits receive tuning");
    f.radio.radioTransmittingChanged(true);
    f.rejected(f.params(), "request.conflict");
    f.radio.radioTransmittingChanged(false);
    f.rejected(f.params(), "request.conflict");
    f.radio.radioTransmitConfirmed(false);
    f.radio.transmitModel().setTransmitting(true);
    f.rejected(f.params(), "request.conflict");
    f.radio.transmitModel().setTransmitting(false);
    f.rejected(f.params(), "request.conflict");
    for (auto signal : {&TransmitModel::moxChanged, &TransmitModel::tuneChanged}) {
        f.radio.radioTransmitConfirmed(false);
        (f.radio.transmitModel().*signal)(true);
        (f.radio.transmitModel().*signal)(false);
        f.rejected(f.params(), "request.conflict");
    }
    f.radio.radioTransmitConfirmed(false);
    f.radio.backendRebuilt();
    f.rejected(f.params(), "request.conflict");
    f.radio.radioTransmitConfirmed(false);
    f.connection.change(RadioConnectionTarget::State::Disconnecting);
    f.rejected(f.params(), "request.conflict");
    f.connection.change(RadioConnectionTarget::State::Connected);
    f.rejected(f.params(), "request.conflict");
    check(f.backend->keys == 0, "safety cases issue no keying");
}

void numericDomainBoundary()
{
    Fixture f;
    constexpr qint64 kWireMaximum = 9'007'199'254'740'991;
    constexpr qint64 kObservationMaximum = SliceModel::kMaximumReportedFrequencyHz;
    check(kWireMaximum - kObservationMaximum == 740'991,
          "documented whole-MHz ceiling remains below the general wire limit");
    f.backend->caps.sliceFrequencyControl.maximumHz = kObservationMaximum;
    check(f.target->available() && f.tune(f.params(kObservationMaximum)).contains("result"),
          "coverage ceiling is inclusive for typed intents");
    f.report(kObservationMaximum);
    check(f.radio.slice(0)->frequencyReportedKnown()
              && f.store.get(f.address)->value.value("frequencyObservation").toObject()
                     .value("hz").toInteger() == kObservationMaximum,
          "coverage ceiling can be published as an exact known observation");
    f.rejected(f.params(kObservationMaximum + 1), "request.out_of_range");
    f.backend->caps.sliceFrequencyControl.maximumHz = kWireMaximum;
    check(!f.target->available(), "wire-valid maximum beyond observation domain disables coverage");
    f.rejected(f.params(), "capability.unavailable");
    f.backend->caps.sliceFrequencyControl.maximumHz = kObservationMaximum;
    f.report(kObservationMaximum + 1'000'000);
    check(!f.radio.slice(0)->frequencyReportedKnown()
              && f.store.get(f.address)->value.value("frequencyObservation").toObject()
                     .value("hz").isNull(),
          "report beyond the shared ceiling clears observation knowledge");
}

void lifetimeAndBinding()
{
    Fixture f;
    const QJsonObject oldSelection = f.params();
    f.backend->connected = false;
    f.radio.connectionStateChanged(false);
    check(!f.store.get(f.address) && !f.target->available(),
          "disconnect removes the selected resource and disables tuning");
    check(f.radio.automationRemoveSliceFixture(0)
              && f.radio.automationApplySliceFixture(0, QStringLiteral("A")),
          "disconnected engine replaces the fixture slice");
    f.backend->connected = true;
    f.radio.connectionStateChanged(true);
    f.report(14'225'000);
    check(f.store.get(f.address).has_value(), "new connection republishes the replacement slice");
    f.rejected(oldSelection, "request.conflict");
    f.backend->connected = false;
    f.rejected(f.params(), "request.conflict");
    f.backend->connected = true;
    bool offThreadAccepted = true;
    std::unique_ptr<QThread> worker(QThread::create([&] {
        offThreadAccepted = !f.target->setFrequency(0, 14'230'000).has_value();
    }));
    worker->start(); worker->wait();
    check(!offThreadAccepted && f.backend->tunes == 0, "wrong-thread production dispatch fails closed");
    check(!f.service.bindFrequencyTarget(f.target.get()), "target cannot be rebound after dispatch");
    f.target.reset();
    f.rejected(f.params(), "capability.unavailable");

    Fixture fresh;
    // Inert production transport objects: neither calls listen() or binds a socket.
    LocalControlServer observer;
    LocalControlServer controller(nullptr, {}, nullptr, true);
    check(!observer.bindFrequencyTarget(fresh.target.get()), "observer transport refuses a target");
    check(controller.bindFrequencyTarget(fresh.target.get()), "opt-in transport accepts initial target");
    check(!controller.bindFrequencyTarget(fresh.target.get()), "transport target binding is one-shot");
    check(!makeModelSliceFrequencyTarget(&fresh.radio, &fresh.connection),
          "factory refuses attachment to an already-live engine");
}

void requestBudget()
{
    Fixture f;
    qint64 now = 0;
    ControlSession limited(&f.store, 65536, SessionAuthorization::Controller,
                           nullptr, [&] { return now; });
    f.hello(limited);
    const QJsonObject params = f.params();
    bool burstAccepted = true;
    for (int i = 0; i < ControlSession::kRequestBurst; ++i) {
        burstAccepted &= call(f.service, limited, "slice.setFrequency", params).contains("result");
    }
    check(burstAccepted && f.backend->tunes == ControlSession::kRequestBurst,
          "control-only grant admits the advertised burst without optimistic revisions");
    check(error(call(f.service, limited, "slice.setFrequency", params)) == "transport.limit_exceeded"
              && f.backend->tunes == ControlSession::kRequestBurst,
          "frequency method cannot bypass the request budget");
    now += 10'000'000'000LL;
    check(error(call(f.service, limited, "slice.setFrequency", params)) == "transport.limit_exceeded",
          "terminal budget exhaustion prevents later dispatch");
    check(f.tune(params).contains("result") && f.backend->tunes == ControlSession::kRequestBurst + 1,
          "a second controller retains its independent budget");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("control-slice-frequency"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    authorizationAndSchema();
    authoritativeObservations();
    coverageAndSafety();
    numericDomainBoundary();
    lifetimeAndBinding();
    requestBudget();
    return failures == 0 ? 0 : 1;
}
