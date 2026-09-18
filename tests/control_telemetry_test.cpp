#include "TestSettingsProfile.h"
#include "core/control/ControlService.h"
#include "core/control/RadioTelemetryAdapter.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

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

class Backend final : public IRadioBackend {
public:
    bool connected{false};
    bool txCapable{false};
    RadioCapabilities capabilities() const override { RadioCapabilities c; c.canTransmit = txCapable; return c; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { check(false, "telemetry must never key"); }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

QJsonObject call(ControlService& service, ControlSession& session, const QString& method, const QJsonObject& params)
{
    QJsonObject request{{"v", 1}, {"id", "test"}, {"method", method}, {"params", params}};
    if (method != QStringLiteral("hello")) { request.insert("sessionId", session.sessionId()); }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
}

struct Fixture {
    RadioModel radio;
    ControlResourceStore store;
    Backend* backend{nullptr};
    qint64 clock{0};
    std::unique_ptr<RadioTelemetryAdapter> adapter;
    Fixture()
    {
        auto owned = std::make_unique<Backend>(); backend = owned.get();
        radio.setBackendForTest(std::move(owned), "telemetry-test");
        adapter = std::make_unique<RadioTelemetryAdapter>(&radio, &store, "radio-1", nullptr, [&] { return clock; });
        backend->connected = true;
        radio.connectionStateChanged(true);
    }
    MeterDef definition(int id) const { return {id, "SLC", id, "LEVEL", "dBm", -140, 0, "Receive level"}; }
    void define(int id) { radio.meterModel().defineMeter(definition(id)); }
    QJsonObject sample(int id) const
    {
        const auto value = store.get({"meter", "radio-1", QString::number(id)});
        return value ? value->value.value("sample").toObject() : QJsonObject{};
    }
    QString transmitState() const { return store.get({"transmitState", "radio-1", {}})->value.value("state").toString(); }
};

void freshnessAndDefinitions()
{
    Fixture f;
    f.define(0);
    f.adapter->flush();
    check(!f.sample(0).value("known").toBool() && f.sample(0).value("value").isNull(),
          "definition without a sample never fabricates zero");
    f.radio.meterModel().updateValues({0}, {-12800});
    f.adapter->flush();
    check(f.sample(0).value("known").toBool() && f.sample(0).value("valid").toBool()
        && f.sample(0).value("value").toDouble() == -100, "physical meter value retains definition units");
    f.clock = 2001; f.adapter->flush();
    check(f.sample(0).value("known").toBool() && !f.sample(0).value("fresh").toBool()
        && f.sample(0).value("value").isNull(), "stale sample is explicitly unknown for current value");
    const auto revision = f.store.get({"meter", "radio-1", "0"})->revision;
    f.clock += 90000; f.adapter->flush();
    check(f.store.get({"meter", "radio-1", "0"})->revision == revision, "expired meter does not publish forever as age grows");
    f.radio.meterModel().updateValues({0}, {-12800}); f.adapter->flush();
    check(f.sample(0).value("fresh").toBool() && f.sample(0).value("ageMs").toInt() == 0,
          "unchanged values refresh sample liveness");
    f.define(0); f.adapter->flush();
    check(f.sample(0).value("known").toBool(), "duplicate identical definitions retain real samples");
    MeterDef changed = f.definition(0); changed.unit = "Watts";
    f.radio.meterModel().defineMeter(changed); f.adapter->flush();
    check(!f.sample(0).value("known").toBool(), "unit redefinition invalidates a legacy retained sample");
    f.radio.meterModel().updateValues({0}, {10}); f.adapter->flush();
    check(f.sample(0).value("value").toInt() == 10, "only a new sample can populate changed units");
    f.radio.meterModel().removeMeter(0);
    check(!f.store.get({"meter", "radio-1", "0"}), "meter removal removes resource");
    f.define(0); f.adapter->flush();
    check(!f.sample(0).value("known").toBool(), "same id recreated does not inherit former sample");
    changed = f.definition(1); changed.description = QString(129, 'x');
    f.radio.meterModel().defineMeter(changed); f.adapter->flush();
    check(!f.store.get({"meter", "radio-1", "1"}) && f.adapter->meterDelivery().value("limited").toBool(),
          "oversized metadata is excluded with incomplete delivery disclosed");
    f.radio.meterModel().clear();
    check(f.store.snapshot({{"meter", "radio-1", {}}}).isEmpty(), "clear removes every published meter");
    f.define(0); f.radio.meterModel().updateValues({0}, {-100}); f.adapter->flush();
    f.backend->connected = false; f.radio.connectionStateChanged(false);
    check(f.store.snapshot({{"meter", "radio-1", {}}}).isEmpty(), "disconnect clears telemetry resources");
    f.radio.meterModel().meterUpdated(0, -40); f.adapter->flush();
    check(f.store.snapshot({{"meter", "radio-1", {}}}).isEmpty(), "late disconnected callback cannot resurrect a resource");
    f.backend->connected = true; f.radio.connectionStateChanged(true); f.adapter->flush();
    check(!f.sample(0).value("known").toBool(), "new connection never imports the former connection's retained sample");
}

void coalescingBoundsAndRevisions()
{
    Fixture f;
    const ResourceAddress slice{"slice", "radio-1", "0"};
    f.store.upsert(slice, {{"mode", "USB"}});
    const quint64 sliceRevision = f.store.get(slice)->revision;
    for (int id = 0; id < RadioTelemetryAdapter::kMaxMeters + 10; ++id) { f.define(id); }
    f.adapter->flush();
    check(f.store.snapshot({{"meter", "radio-1", {}}}).size() == RadioTelemetryAdapter::kMaxMeters
        && f.adapter->meterDelivery().value("limited").toBool(), "resource population is hard bounded and overflow disclosed");
    ControlService service(&f.store);
    ControlSession client(&f.store, 262144, SessionAuthorization::Observer);
    check(call(service, client, "hello", {{"versions", QJsonArray{1}}}).contains("result"), "telemetry observer negotiated");
    const QJsonArray selectors{QJsonObject{{"type", "meter"}, {"radioSession", "radio-1"}},
        QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}},
        QJsonObject{{"type", "transmitState"}, {"radioSession", "radio-1"}}};
    const auto baseline = call(service, client, "resource.subscribe", {{"resources", selectors}});
    check(baseline.contains("result") && QJsonDocument(baseline).toJson(QJsonDocument::Compact).size() < ProtocolLimits::kMaxMessageBytes,
          "complete bounded meter/slice/transmit baseline fits wire cap");
    for (int i = 0; i < 10000; ++i) { f.radio.meterModel().meterUpdated(0, float(i)); }
    check(client.takePendingFrames().isEmpty(), "high-rate samples do not publish from sample callback");
    f.adapter->flush();
    const auto frames = client.takePendingFrames();
    check(frames.size() == 1 && f.sample(0).value("value").toDouble() == 9999, "one tick publishes only bounded latest value");
    check(f.store.get(slice)->revision == sliceRevision, "telemetry never invalidates independent receive-command revision");
    ControlSession slow(&f.store, 360, SessionAuthorization::Observer);
    call(service, slow, "hello", {{"versions", QJsonArray{1}}});
    call(service, slow, "resource.subscribe", {{"resources", selectors}});
    f.radio.meterModel().meterUpdated(0, -90); f.adapter->flush();
    const auto overflow = slow.takePendingFrames();
    check(overflow.size() == 1 && QJsonDocument::fromJson(overflow.first()).object().value("event") == "resource.resyncRequired",
          "slow telemetry consumer uses existing bounded resync policy");
    client.revokeAuthorization();
    f.radio.meterModel().meterUpdated(0, -80); f.adapter->flush();
    check(client.takePendingFrames().isEmpty(), "revocation discards pending telemetry and suppresses future delivery");
}

void snapshotRefusalIsAtomic()
{
    ControlResourceStore store;
    ControlService service(&store);
    ControlSession client(&store, 1024 * 1024, SessionAuthorization::Observer);
    call(service, client, "hello", {{"versions", QJsonArray{1}}});
    for (int id = 0; id < 5; ++id) {
        store.upsert({"meter", "radio-1", QString::number(id)}, {{"large", QString(65536, 'x')}});
    }
    const auto reply = call(service, client, "resource.subscribe", {{"resources", QJsonArray{
        QJsonObject{{"type", "meter"}, {"radioSession", "radio-1"}}}}});
    check(reply.value("error").toObject().value("code") == "transport.limit_exceeded", "oversized atomic snapshot refused within wire limit");
    store.upsert({"meter", "radio-1", "0"}, {{"small", true}});
    check(client.takePendingFrames().isEmpty(), "failed baseline installs no invisible subscription");
    const auto narrow = call(service, client, "resource.subscribe", {{"resources", QJsonArray{
        QJsonObject{{"type", "meter"}, {"radioSession", "radio-1"}, {"id", "0"}}}}});
    check(narrow.contains("result") && narrow.value("result").toObject().value("subscription") == "sub-1",
          "narrow baseline succeeds after refusal without consuming subscription identity");
}

void transmitIsObservationOnly()
{
    Fixture f;
    check(f.transmitState() == "unsupported", "RX-only backend reports unsupported, not confirmed TX idle");
    f.backend->txCapable = true; f.radio.capabilitiesChanged(true, f.backend->capabilities());
    check(f.transmitState() == "unknown", "constructor defaults cannot fabricate transmitter idle");
    f.radio.radioTransmitConfirmed(false);
    check(f.transmitState() == "idle", "only confirmed backend idle makes state idle");
    f.radio.transmitModel().moxChanged(true); f.radio.transmitModel().moxChanged(false);
    check(f.transmitState() == "unknown", "command-off edge cannot establish idle");
    f.radio.radioTransmitConfirmed(true);
    check(f.transmitState() == "transmitting", "confirmed active transmit state can be observed");
    f.radio.backendRebuilt();
    check(f.transmitState() == "unknown", "backend lifetime edge invalidates transmit knowledge");
    ControlService service(&f.store);
    ControlSession client(&f.store, 262144, SessionAuthorization::ObserverController);
    call(service, client, "hello", {{"versions", QJsonArray{1}}});
    check(!service.capabilities(client).value("grants").toArray().contains("transmit"), "no transmit grant is representable");
    check(call(service, client, "transmit.set", {{"enabled", true}}).value("error").toObject().value("code") == "request.unknown_method",
          "read-only transmit resource exposes no TX command");
}

void admissionBackfillsValidDefinitions()
{
    Fixture f;
    f.backend->connected = false;
    f.radio.connectionStateChanged(false);
    for (int id = 0; id < 130; ++id) {
        MeterDef def = f.definition(id);
        if (id < 64) { def.description = QString(129, 'x'); }
        f.radio.meterModel().defineMeter(def);
    }
    f.backend->connected = true;
    f.radio.connectionStateChanged(true);
    f.adapter->flush();
    check(f.store.snapshot({{"meter", "radio-1", {}}}).size() == 64
        && f.store.get({"meter", "radio-1", "127"}),
        "initial admission fills 64 valid slots past invalid early definitions");
    f.radio.meterModel().updateValues({128}, {-12800});
    f.radio.meterModel().removeMeter(64);
    f.adapter->flush();
    check(f.store.snapshot({{"meter", "radio-1", {}}}).size() == 64
        && f.store.get({"meter", "radio-1", "128"}) && !f.sample(128).value("known").toBool(),
        "removal promotes excluded definition without importing its retained sample");
    MeterDef invalid = f.definition(65); invalid.name.clear();
    f.radio.meterModel().defineMeter(invalid);
    f.adapter->flush();
    check(!f.store.get({"meter", "radio-1", "65"}) && f.store.get({"meter", "radio-1", "129"})
        && f.store.snapshot({{"meter", "radio-1", {}}}).size() == 64,
        "invalidating a selected definition also fills the vacancy");
    check(f.adapter->meterDelivery().value("limited").toBool(), "backfill does not erase incomplete-view history");
}

void unicodeAndMeterAdvertisement()
{
    Fixture f;
    ControlService service(&f.store);
    ControlSession client(&f.store, 262144, SessionAuthorization::Observer);
    call(service, client, "hello", {{"versions", QJsonArray{1}}});
    const auto advertised = [&] { return service.capabilities(client).value("capabilities").toArray(); };
    check(advertised().contains("transmitState.read") && !advertised().contains("meter.read"),
        "transmit singleton does not advertise absent meter resources");
    for (const QString& bad : {QString(QChar(0x202e)), QString(QChar(0x2066)),
                              QString::fromUcs4(U"\U000e0001"), QString(QChar(0x0009))}) {
        MeterDef def = f.definition(0); def.description = bad;
        f.radio.meterModel().defineMeter(def); f.adapter->flush();
        check(!f.store.get({"meter", "radio-1", "0"}), "Unicode control/format metadata is excluded, including supplementary characters");
    }
    MeterDef valid = f.definition(0); valid.description = QString::fromUtf8("Signal — 接收");
    f.radio.meterModel().defineMeter(valid); f.adapter->flush();
    check(f.store.get({"meter", "radio-1", "0"}).has_value() && advertised().contains("meter.read"),
        "ordinary Unicode definition is readable before its first sample");
    f.radio.meterModel().removeMeter(0);
    check(!advertised().contains("meter.read"), "last meter removal retires its advertisement");
}

void delayedIdleCannotSurviveActivity()
{
    for (int kind = 0; kind < 4; ++kind) {
        Fixture f;
        f.backend->txCapable = true;
        f.radio.capabilitiesChanged(true, f.backend->capabilities());
        const auto activity = [&](bool active) {
            if (kind == 0) {
                f.radio.transmitModel().setTransmitting(active);
            } else {
                TransmitDelta delta;
                if (kind == 2) { delta.tune = active; } else { delta.mox = active; }
                if (kind == 3) {
                    f.radio.handleStatusForTest("interlock", {{"state", active ? "TRANSMITTING" : "READY"}});
                }
                else { f.radio.transmitModel().applyChanges(delta); }
                if (kind == 1) { f.radio.transmitModel().moxChanged(active); }
            }
        };
        f.radio.radioTransmitConfirmed(false);
        activity(true);
        f.radio.radioTransmitConfirmed(false);
        check(f.transmitState() == "unknown", "conflicting idle is not telemetry evidence during activity");
        activity(false);
        check(f.transmitState() == "unknown", "falling activity edge cannot revive delayed idle telemetry");
        f.radio.radioTransmitConfirmed(false);
        check(f.transmitState() == "idle", "fresh post-activity idle restores telemetry");
    }
}

void teardownDoesNotRepublish()
{
    Fixture f;
    f.define(0);
    f.adapter->flush();
    MeterDef invalid = f.definition(1);
    invalid.description = QString(129, 'x');
    f.radio.meterModel().defineMeter(invalid);
    check(f.adapter->meterDelivery().value("limited").toBool(), "teardown starts with limited delivery");
    int publications = 0;
    QObject::connect(f.adapter.get(), &RadioTelemetryAdapter::deliveryChanged, &f.store,
                     [&] { ++publications; });
    f.adapter.reset();
    check(publications == 0, "destruction never calls the partially destroyed projection owner");
    check(f.store.snapshot({{"meter", "radio-1", {}}}).isEmpty()
        && !f.store.get({"transmitState", "radio-1", {}}), "teardown still retires telemetry resources");
}

void swrRequiresApplicablePower()
{
    Fixture f;
    f.radio.meterModel().defineMeter({0, "TX", 0, "SWR", "SWR", 1, 100, "Standing wave ratio"});
    f.radio.meterModel().defineMeter({1, "TX", 0, "FWDPWR", "Watts", 0, 100, "Forward power"});
    f.radio.meterModel().updateValues({0, 1}, {256, 10});
    f.adapter->flush();
    check(f.sample(0).value("valid").toBool() && f.sample(0).value("value").toDouble() == 2,
          "fresh SWR with qualifying power is a valid physical sample");
    f.radio.meterModel().updateValues({1}, {0});
    f.adapter->flush();
    check(f.sample(0).value("fresh").toBool() && !f.sample(0).value("valid").toBool()
        && f.sample(0).value("value").isNull(), "fresh SWR without power never looks like a healthy ratio");
    f.radio.meterModel().updateValues({0, 1}, {256, 10});
    f.radio.meterModel().setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - 3000);
    f.adapter->flush();
    check(!f.sample(0).value("valid").toBool(), "the model's own SWR freshness gate remains authoritative");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("control-telemetry"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    freshnessAndDefinitions();
    coalescingBoundsAndRevisions();
    snapshotRefusalIsAtomic();
    transmitIsObservationOnly();
    teardownDoesNotRepublish();
    swrRequiresApplicablePower();
    admissionBackfillsValidDefinitions();
    unicodeAndMeterAdvertisement();
    delayedIdleCannotSurviveActivity();
    return failures == 0 ? 0 : 1;
}
