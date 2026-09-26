#include "TestSettingsProfile.h"
#include "core/control/ControlService.h"
#include "core/control/LocalControlServer.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/sim/SimBackend.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#endif

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

// Injected intent recorder, not a firmware simulator: observations are fed
// separately. No socket, discovery, DSP worker, synthetic peer or TX path.
class Backend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    int intents{0};
    int keys{0};
    QString method;
    QString panId;
    QJsonObject args;
    mutable int reads{0};
    RadioCapabilities capabilities() const override { ++reads; return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double hz) override { record("frequency", {{"hz", hz}}); }
    void setSliceMode(int id, const QString& mode) override { record("mode", {{"slice", id}, {"mode", mode}}); }
    void setSliceFilter(int id, int low, int high) override { record("filter", {{"slice", id}, {"low", low}, {"high", high}}); }
    void setSliceAudioGain(int id, int gain) override { record("gain", {{"slice", id}, {"gain", gain}}); }
    void setSliceAudioMute(int id, bool muted) override { record("mute", {{"slice", id}, {"muted", muted}}); }
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString& id, double hz, PanCenterIntent) override { panId = id; record("center", {{"hz", hz}}); }
    void setPanBandwidth(const QString& id, double hz) override { panId = id; record("bandwidth", {{"hz", hz}}); }
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&, const AetherSDR::TxCoordinator::Completion&) override { ++keys; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void record(const QString& name, const QJsonObject& values) { ++intents; method = name; args = values; }
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
                 const QJsonObject& params = {})
{
    QJsonObject request{{"v", 1}, {"id", "request"}, {"method", method}, {"params", params}};
    if (method != QStringLiteral("hello")) {
        request.insert("sessionId", session.sessionId());
    }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
}

QString error(const QJsonObject& response) { return response.value("error").toObject().value("code").toString(); }
const QString kBackendPan = QStringLiteral("receiver-main");

struct Fixture {
    RadioModel radio;
    Connection connection;
    ControlResourceStore store;
    std::unique_ptr<ReceiveControlTarget> target;
    ControlService service{&store};
    ControlSession controller{&store, 256 * 1024, SessionAuthorization::ObserverController};
    RadioResourceAdapter adapter{&radio, &store, QStringLiteral("radio-1"), nullptr, &connection};
    Backend* backend{nullptr};
    const QString pan = RadioModel::neutralPanIdStringForTest(0);
    const ResourceAddress sliceAddress{QStringLiteral("slice"), QStringLiteral("radio-1"), QStringLiteral("0")};
    const ResourceAddress panAddress{QStringLiteral("panadapter"), QStringLiteral("radio-1"), pan};

    Fixture()
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        using Authority = SliceFrequencyControl::Authority;
        backend->caps.canTransmit = false;
        backend->caps.receiveModeControl = ReceiveModeControl{Authority::Engine, {"USB", "LSB", "AM"}};
        backend->caps.receiveFilterControl = ReceiveFilterControl{Authority::Engine, {
            {"USB", 0, 11990, 10, 12000, 10, 12000},
            {"LSB", -12000, -10, -11990, 0, 10, 12000},
            {"AM", -12000, -10, 10, 12000, 20, 24000}}};
        backend->caps.receiveAudioControl = ReceiveAudioControl{Authority::Engine};
        backend->caps.receivePanCenterControl = ReceivePanRangeControl{Authority::Engine, 100000, 60000000};
        backend->caps.receivePanBandwidthControl = ReceivePanRangeControl{Authority::Engine, 48000, 1536000};
        radio.setBackendForTest(std::move(owned), QStringLiteral("receive-test"));
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "socket-free owned slice installed");
        target = makeModelReceiveControlTarget(&radio, &connection);
        check(target && service.bindReceiveTarget(target.get()), "receive target bound before connection and negotiation");
        backend->connected = true;
        connection.change(RadioConnectionTarget::State::Connected);
        radio.connectionStateChanged(true);
        report("USB", 100, 2800);
        SliceDelta audio; audio.audioGain = 50; audio.audioMute = false;
        radio.slice(0)->applyChanges(audio);
        backend->panCenterBandwidthChanged(kBackendPan, 14.225, .192);
        adapter.publishAll();
        hello(controller);
    }

    void hello(ControlSession& session)
    {
        check(call(service, session, "hello", {{"versions", QJsonArray{1}}}).contains("result"), "session negotiated");
    }
    void report(const QString& mode, int low, int high)
    {
        SliceDelta delta; delta.mode = mode; delta.filterLow = low; delta.filterHigh = high;
        radio.slice(0)->applyChanges(delta);
    }
    QJsonObject params(ReceiveOperation op) const
    {
        const bool isPan = op == ReceiveOperation::PanCenter || op == ReceiveOperation::PanBandwidth;
        const auto snapshot = store.get(isPan ? panAddress : sliceAddress);
        QJsonObject values{{"radioSession", "radio-1"}, {isPan ? "panadapter" : "slice", isPan ? pan : "0"},
            {"expectedRevision", static_cast<qint64>(snapshot ? snapshot->revision : 0)}};
        switch (op) {
        case ReceiveOperation::Mode: values.insert("mode", "LSB"); break;
        case ReceiveOperation::Filter: values.insert("lowHz", 200); values.insert("highHz", 2900); break;
        case ReceiveOperation::AudioGain: values.insert("gain", 35); break;
        case ReceiveOperation::AudioMute: values.insert("muted", true); break;
        case ReceiveOperation::PanCenter: values.insert("hz", 14230000); break;
        case ReceiveOperation::PanBandwidth: values.insert("hz", 96000); break;
        }
        return values;
    }
    QJsonObject send(ReceiveOperation op, const QJsonObject& values)
    {
        for (const auto& [operation, method] : kReceiveMethods) {
            if (operation == op) { return call(service, controller, QString::fromLatin1(method), values); }
        }
        return {};
    }
    void reject(ReceiveOperation op, const QJsonObject& values, const char* code)
    {
        const int before = backend->intents;
        const QString actual = error(send(op, values));
        if (actual != QString::fromLatin1(code)) { std::printf("Expected %s, got %s\n", code, qPrintable(actual)); }
        check(actual == QString::fromLatin1(code) && backend->intents == before,
              "refusal has expected code and dispatches no backend intent");
    }
};

void schemaAndAuthorization()
{
    for (const auto& [op, method] : kReceiveMethods) {
        Fixture f;
        for (SessionAuthorization auth : {SessionAuthorization::Observer, SessionAuthorization::AuthenticatedWithoutGrants}) {
            ControlSession denied(&f.store, 262144, auth);
            f.hello(denied);
            check(error(call(f.service, denied, QString::fromLatin1(method), {{"invalid", true}})) == "auth.grant_denied",
                  "every receive verb checks grants before schema or target");
            check(!f.service.capabilities(denied).value("capabilities").toArray().contains(QString::fromLatin1(method)),
                  "observer never advertises receive mutations");
        }
        const QJsonObject good = f.params(op);
        QJsonObject bad = good; bad.insert("force", true);
        f.reject(op, bad, "request.invalid_params");
        for (const QString& field : good.keys()) {
            bad = good; bad.remove(field); f.reject(op, bad, "request.invalid_params");
        }
        for (QJsonValue value : {QJsonValue(0), QJsonValue(1.5), QJsonValue(true), QJsonValue("3"), QJsonValue(9007199254740992.0)}) {
            bad = good; bad.insert("expectedRevision", value); f.reject(op, bad, "request.invalid_params");
        }
        bad = good; bad.insert("radioSession", "radio-2"); f.reject(op, bad, "resource.not_found");
        bad = good; bad.insert(good.contains("slice") ? "slice" : "panadapter", "missing");
        f.reject(op, bad, good.contains("slice") ? "request.invalid_params" : "resource.not_found");
        if (good.contains("slice")) {
            for (const char* id : {"-1", "+0", "00", "2147483648", "0\nraw"}) {
                bad = good; bad.insert("slice", id); f.reject(op, bad, "request.invalid_params");
            }
        }
        f.controller.revokeAuthorization();
        f.reject(op, good, "auth.invalid");
    }
    Fixture f;
    for (QJsonValue mode : {QJsonValue("usb"), QJsonValue("USB\ntransmit"), QJsonValue(""), QJsonValue(true), QJsonValue(QString(33, 'A'))}) {
        auto bad = f.params(ReceiveOperation::Mode); bad.insert("mode", mode);
        f.reject(ReceiveOperation::Mode, bad, "request.invalid_params");
    }
    for (ReceiveOperation op : {ReceiveOperation::Filter, ReceiveOperation::AudioGain, ReceiveOperation::PanCenter, ReceiveOperation::PanBandwidth}) {
        const char* field = op == ReceiveOperation::Filter ? "lowHz" : op == ReceiveOperation::AudioGain ? "gain" : "hz";
        for (QJsonValue value : {QJsonValue(true), QJsonValue("12"), QJsonValue(1.5), QJsonValue(QJsonValue::Null)}) {
            auto bad = f.params(op); bad.insert(field, value); f.reject(op, bad, "request.invalid_params");
        }
    }
    for (QJsonValue value : {QJsonValue(0), QJsonValue(1), QJsonValue("true"), QJsonValue(QJsonValue::Null)}) {
        auto bad = f.params(ReceiveOperation::AudioMute); bad.insert("muted", value);
        f.reject(ReceiveOperation::AudioMute, bad, "request.invalid_params");
    }
}

void dispatchAndReadback()
{
    for (const auto& [op, method] : kReceiveMethods) {
        Fixture f;
        const bool pan = op == ReceiveOperation::PanCenter || op == ReceiveOperation::PanBandwidth;
        const ResourceAddress address = pan ? f.panAddress : f.sliceAddress;
        const auto before = *f.store.get(address);
        check(f.service.capabilities(f.controller).value("capabilities").toArray().contains(QString::fromLatin1(method)),
              "qualified operation advertised from action-time capability and observation");
        check(f.send(op, f.params(op)).value("result").toObject().value("accepted").toBool() && f.backend->intents == 1,
              "accepted dispatches exactly one typed intent");
        check(f.store.get(address)->revision == before.revision && f.store.get(address)->value == before.value,
              "acceptance neither fabricates readback nor advances revision");
        if (pan) {
            check(f.backend->panId == kBackendPan, "opaque model pan id resolves to exact backend id");
            check(f.backend->method != "frequency", "pan command does not implicitly tune a slice");
        }
        check(f.backend->keys == 0, "receive verbs never invoke keying");
    }
    Fixture f;
    ControlSession observer(&f.store, 262144, SessionAuthorization::Observer);
    f.hello(observer);
    call(f.service, observer, "resource.subscribe", {{"resources", QJsonArray{
        QJsonObject{{"type", "slice"}, {"radioSession", "radio-1"}}}}});
    const auto stale = f.params(ReceiveOperation::AudioGain);
    const int beforeReads = f.backend->reads;
    SliceDelta delta; delta.audioGain = 35; delta.audioMute = true;
    f.radio.slice(0)->applyChanges(delta);
    check(f.backend->reads == beforeReads, "receive readback does not rebuild capabilities per sample");
    check(!observer.takePendingFrames().isEmpty(), "independent observer receives authoritative receive update");
    f.reject(ReceiveOperation::AudioGain, stale, "request.conflict");
    const auto revision = f.store.get(f.sliceAddress)->revision;
    f.radio.slice(0)->applyChanges(delta);
    check(f.store.get(f.sliceAddress)->revision == revision, "same-value receive echoes do not churn revisions");
    f.radio.slice(0)->setAudioGain(60);
    check(f.radio.slice(0)->receiveObservation().gain == 35, "desktop optimistic gain is not an observation");
    delta.audioGain = 60; f.radio.slice(0)->applyChanges(delta);
    check(f.radio.slice(0)->receiveObservation().gain == 60, "echo equal to optimistic gain still establishes readback");
    delta.audioGain = std::numeric_limits<double>::infinity(); f.radio.slice(0)->applyChanges(delta);
    check(!f.target->available(ReceiveOperation::AudioGain), "non-finite gain observation cannot enable control");
}

void modeAndFilterOrdering()
{
    Fixture f;
    check(f.send(ReceiveOperation::Mode, f.params(ReceiveOperation::Mode)).contains("result"), "mode intent accepted");
    auto competing = f.params(ReceiveOperation::Mode); competing.insert("mode", "AM");
    f.reject(ReceiveOperation::Mode, competing, "request.conflict");
    f.reject(ReceiveOperation::Filter, f.params(ReceiveOperation::Filter), "request.conflict");
    f.report("USB", 100, 2800);
    f.reject(ReceiveOperation::Filter, f.params(ReceiveOperation::Filter), "request.conflict");
    SliceDelta mode; mode.mode = "LSB"; f.radio.slice(0)->applyChanges(mode);
    check(!f.target->available(ReceiveOperation::Filter), "mode-only readback invalidates old-mode filter edges");
    SliceDelta low; low.filterLow = -2800; f.radio.slice(0)->applyChanges(low);
    check(!f.target->available(ReceiveOperation::Filter), "partial filter readback does not invent its other edge");
    SliceDelta high; high.filterHigh = -100; f.radio.slice(0)->applyChanges(high);
    auto filter = f.params(ReceiveOperation::Filter); filter.insert("lowHz", -2900); filter.insert("highHz", -200);
    check(f.send(ReceiveOperation::Filter, filter).contains("result"), "new-mode passband dispatches after full observation");
    for (const auto& edges : {std::pair{200, 2900}, std::pair{-100, -200}, std::pair{-12001, -1}, std::pair{-5, 0}}) {
        filter = f.params(ReceiveOperation::Filter); filter.insert("lowHz", edges.first); filter.insert("highHz", edges.second);
        f.reject(ReceiveOperation::Filter, filter, "request.out_of_range");
    }
    auto same = f.params(ReceiveOperation::Mode); same.insert("mode", "LSB");
    check(f.send(ReceiveOperation::Mode, same).contains("result"), "same-value mode remains a valid intent");
    mode.mode = "LSB"; f.radio.slice(0)->applyChanges(mode);
    check(f.target->available(ReceiveOperation::Filter), "same-value mode report releases pending filter interlock");
    same = f.params(ReceiveOperation::Mode); same.insert("mode", "RADE");
    f.reject(ReceiveOperation::Mode, same, "request.out_of_range");
    f.report("RADE", -3000, 3000);
    f.reject(ReceiveOperation::Mode, f.params(ReceiveOperation::Mode), "capability.unavailable");
}

void delayedIdleCannotAuthorizeReceive()
{
    for (int kind = 0; kind < 4; ++kind) {
        Fixture f;
        f.backend->caps.canTransmit = true;
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
        f.reject(ReceiveOperation::AudioGain, f.params(ReceiveOperation::AudioGain), "request.conflict");
        activity(false);
        f.reject(ReceiveOperation::AudioGain, f.params(ReceiveOperation::AudioGain), "request.conflict");
        f.radio.radioTransmitConfirmed(false);
        check(f.send(ReceiveOperation::AudioGain, f.params(ReceiveOperation::AudioGain)).contains("result"),
              "only fresh post-activity idle reopens receive admission");
        check(f.backend->keys == 0, "injected activity never dispatches keying");
    }
}

void safetyAndLifetime()
{
    for (const auto& [op, method] : kReceiveMethods) {
        Q_UNUSED(method);
        Fixture f;
        const auto old = f.params(op);
        f.backend->caps.canTransmit = true;
        f.reject(op, old, "request.conflict");
        f.radio.radioTransmitConfirmed(false);
        check(f.send(op, f.params(op)).contains("result"), "explicit idle readback permits non-TX control");
        f.radio.transmitModel().moxChanged(true);
        f.radio.transmitModel().moxChanged(false);
        f.reject(op, f.params(op), "request.conflict");
        f.radio.radioTransmitConfirmed(false);
        f.connection.change(RadioConnectionTarget::State::Disconnecting);
        f.connection.change(RadioConnectionTarget::State::Connected);
        f.reject(op, f.params(op), "request.conflict");
        f.backend->caps.canTransmit = false;
        f.backend->connected = false;
        f.reject(op, f.params(op), "request.conflict");
        f.backend->connected = true;
        bool accepted = true;
        auto worker = std::unique_ptr<QThread>(QThread::create([&] {
            accepted = !f.target->setAudioGain(0, 30).has_value();
        }));
        worker->start(); worker->wait();
        check(!accepted, "wrong-thread dispatch fails closed");
        check(!f.service.bindReceiveTarget(f.target.get()), "service cannot replace target after dispatch");
        f.target.reset();
        f.reject(op, f.params(op), "capability.unavailable");
        check(f.backend->keys == 0, "safety and lifetime failures never key");
    }
    Fixture f;
    f.radio.slice(0)->setLocked(true);
    f.reject(ReceiveOperation::AudioGain, f.params(ReceiveOperation::AudioGain), "request.conflict");
    f.radio.slice(0)->setLocked(false);
    const auto old = f.params(ReceiveOperation::AudioGain);
    f.radio.slice(0)->invalidateFrequencyObservation();
    check(!f.target->available(ReceiveOperation::Mode) && !f.target->available(ReceiveOperation::Filter)
        && !f.target->available(ReceiveOperation::AudioGain) && !f.target->available(ReceiveOperation::AudioMute),
        "reconnect invalidates every receive observation, not just frequency");
    f.reject(ReceiveOperation::AudioGain, old, "request.conflict");
    f.radio.panadapter(f.pan)->resetCenterKnownForReconnect();
    check(!f.target->available(ReceiveOperation::PanCenter), "reconnect invalidates geometry knowledge");
    f.backend->panCenterBandwidthChanged(kBackendPan, 14.225, -1);
    check(!f.target->available(ReceiveOperation::PanCenter), "partial pan report cannot fill missing bandwidth");
    f.backend->panCenterBandwidthChanged(kBackendPan, -1, .192);
    check(f.target->available(ReceiveOperation::PanCenter), "split pan reports establish complete geometry");
    f.radio.panadapter(f.pan)->setClientHandle("0x1234");
    check(!f.target->available(ReceiveOperation::PanCenter), "foreign owner revokes pan eligibility");
    check(!f.store.get(f.panAddress)->value.value("owned").toBool(), "ownership change republishes after new owner is installed");
}

void rangesAndBudget()
{
    Fixture f;
    for (int gain : {-1, 101}) {
        auto params = f.params(ReceiveOperation::AudioGain); params.insert("gain", gain);
        f.reject(ReceiveOperation::AudioGain, params, "request.out_of_range");
    }
    for (int gain : {0, 100}) {
        auto params = f.params(ReceiveOperation::AudioGain); params.insert("gain", gain);
        check(f.send(ReceiveOperation::AudioGain, params).contains("result"), "gain bounds inclusive");
    }
    for (ReceiveOperation op : {ReceiveOperation::PanCenter, ReceiveOperation::PanBandwidth}) {
        auto params = f.params(op); params.insert("hz", 1);
        f.reject(op, params, "request.out_of_range");
        params.insert("hz", 60000001); f.reject(op, params, "request.out_of_range");
    }
    f.backend->caps.receiveAudioControl.reset();
    f.reject(ReceiveOperation::AudioMute, f.params(ReceiveOperation::AudioMute), "capability.unavailable");
    f.backend->caps.receivePanCenterControl->authority = SliceFrequencyControl::Authority::Unknown;
    f.reject(ReceiveOperation::PanCenter, f.params(ReceiveOperation::PanCenter), "capability.unavailable");
    qint64 now = 0;
    ControlSession limited(&f.store, 262144, SessionAuthorization::Controller, nullptr, [&] { return now; });
    f.hello(limited);
    const int before = f.backend->intents;
    bool accepted = true;
    for (int i = 0; i < ControlSession::kRequestBurst; ++i) {
        accepted &= call(f.service, limited, "panadapter.setBandwidth", f.params(ReceiveOperation::PanBandwidth)).contains("result");
    }
    check(accepted && f.backend->intents == before + ControlSession::kRequestBurst, "receive verbs share existing burst budget");
    check(error(call(f.service, limited, "slice.setMode", f.params(ReceiveOperation::Mode))) == "transport.limit_exceeded",
          "switching methods cannot escape shared request budget");
}

void productionCapabilityContracts()
{
    // Constructors only: never connect a backend or start DSP/discovery. These
    // pin declarations and the HL2 pre-connect normalized state, not RF effects.
    FlexBackend flex;
    const auto flexCaps = flex.capabilities();
    check(flexCaps.receiveModeControl && flexCaps.receiveFilterControl
        && !flexCaps.receiveAudioControl && !flexCaps.receivePanCenterControl && !flexCaps.receivePanBandwidthControl,
        "Flex leaves legacy coupled geometry/audio unadvertised");
    bool unsafeFlexFilter = false;
    for (const ReceiveFilterMode& mode : flexCaps.receiveFilterControl->modes) {
        unsafeFlexFilter |= mode.mode == "CW" || mode.mode == "FM" || mode.mode == "NFM" || mode.mode == "RTTY";
    }
    check(!unsafeFlexFilter, "pitch/preset/shift-dependent Flex filters are not guessed");
    QStringList flexCommands;
    flex.setSliceCommandSink([&](const QString& command) { flexCommands.append(command); });
    flex.setSliceMode(0, "LSB");
    flex.setSliceFilter(0, -2800, -100);
    check(flexCommands == QStringList{"slice set 0 mode=LSB", "filt 0 -2800 -100"},
        "Flex records have implemented typed verbs behind an injected command sink");
    SimBackend sim;
    const auto simCaps = sim.capabilities();
    check(simCaps.receiveModeControl && simCaps.receiveModeControl->modes == QStringList{"USB", "LSB"}
        && !simCaps.receiveFilterControl && !simCaps.receiveAudioControl
        && !simCaps.receivePanCenterControl && !simCaps.receivePanBandwidthControl,
        "Demo exposes only real sideband behavior, never echo-only filter or fixed scene geometry");
    hl2::Hl2Backend hl2;
    const auto hl2Caps = hl2.capabilities();
    check(hl2Caps.receiveModeControl && hl2Caps.receiveFilterControl && hl2Caps.receiveAudioControl
        && hl2Caps.receivePanCenterControl && !hl2Caps.receivePanBandwidthControl,
        "HL2 exposes receive controls but not topology-changing bandwidth");
    SliceDelta observed;
    int gainReports = 0;
    QObject::connect(&hl2, &IRadioBackend::sliceChanged, &hl2, [&](int, const SliceDelta& delta) {
        observed = delta;
        ++gainReports;
    });
    hl2.setSliceAudioGain(0, 37);
    check(observed.audioGain == 37, "HL2 publishes backend mixer gain as normalized observation");
    const int beforeDuplicate = gainReports;
    hl2.setSliceAudioGain(0, 37);
    check(gainReports == beforeDuplicate, "duplicate HL2 gain does not republish the full slice");
    hl2.setSliceAudioGain(0, 150);
    check(observed.audioGain == 100 && gainReports == beforeDuplicate + 1, "changed HL2 gain is clamped and reported");
    hl2.setSliceAudioGain(0, 101);
    check(gainReports == beforeDuplicate + 1, "equal clamped HL2 gain is also change gated");
    hl2.setSliceAudioGain(0, 37);
    hl2.setSliceAudioMute(0, true);
    check(observed.audioMute == true && observed.audioGain == 37, "HL2 mute readback preserves mixer gain");
    hl2.setSliceMode(0, "LSB");
    check(observed.mode == QStringLiteral("LSB") && observed.filterLow == -2900 && observed.filterHigh == -100,
        "HL2 reports new-mode default passband with mode");
    hl2.setSliceFilter(0, -2500, -200);
    check(observed.filterLow == -2500 && observed.filterHigh == -200, "HL2 filter intent reports backend-owned passband");
    // DSB AND THE TWO CW SPELLINGS ARE MODES THIS RADIO DEMODULATES, and the
    // declaration omitted them. This is not only "unreachable through the
    // control plane": ModelReceiveControlTarget reads this list TWICE, and
    // checkSlice() -- the FIRST statement of its setMode() -- requires the
    // slice's OBSERVED mode to be in it. So a slice sitting in DSB or CWL had
    // slice.setMode refused whatever mode was asked for, including a listed
    // one: no way back out, not merely no way in.
    //
    // FM is on the list for a THIRD reason, and the one that made it urgent:
    // publishedModeStrings() carries "FM", so SliceDelta::modeList publishes
    // it and the mode COMBO offers it. A mode an operator can select and that
    // this list omits is a slice that cannot be steered back out at all.
    check(hl2Caps.receiveModeControl->modes.contains(QStringLiteral("DSB"))
        && hl2Caps.receiveModeControl->modes.contains(QStringLiteral("CWL"))
        && hl2Caps.receiveModeControl->modes.contains(QStringLiteral("CW"))
        && hl2Caps.receiveModeControl->modes.contains(QStringLiteral("FM")),
        "HL2 declares DSB, CWL and FM, which its demodulator has");
    // THE MENU AND THE CONTROL PLANE AGREE, BOTH WAYS. Derived from
    // hl2::publishedModeStrings() rather than retyped, so a mode added to the
    // menu later fails here instead of stranding an operator, and a spelling
    // added to this list that the menu cannot show fails here too.
    //
    // The two directions catch different faults and are reported separately:
    //   * offered-but-undeclared is the STRANDING -- checkSlice() reads the
    //     OBSERVED mode, so any mode the combo can select must be on the list.
    //   * declared-but-unoffered is the WEDGE below -- a spelling that can be
    //     REQUESTED but that the backend will rewrite before publishing.
    QStringList offeredButUndeclared;
    for (const QString& offered : hl2::publishedModeStrings()) {
        if (!hl2Caps.receiveModeControl->modes.contains(offered))
            offeredButUndeclared.append(offered);
    }
    QStringList declaredButUnoffered;
    for (const QString& declared : hl2Caps.receiveModeControl->modes) {
        if (!hl2::publishedModeStrings().contains(declared))
            declaredButUnoffered.append(declared);
    }
    if (!offeredButUndeclared.isEmpty() || !declaredButUnoffered.isEmpty()) {
        std::printf("menu-only: %s | control-plane-only: %s\n",
                    qPrintable(offeredButUndeclared.join(QLatin1Char(','))),
                    qPrintable(declaredButUnoffered.join(QLatin1Char(','))));
    }
    check(offeredButUndeclared.isEmpty(),
        "every mode the HL2 mode MENU offers is steerable -- no menu entry strands a slice");
    check(declaredButUnoffered.isEmpty(),
        "and nothing is requestable that the menu cannot display");
    // THE WEDGE, PINNED AS AN INVARIANT RATHER THAN AS A LIST. Every mode on
    // receiveModeControl must be its OWN canonical spelling.
    //
    // Why this is not tidiness. ModelReceiveControlTarget::setMode records the
    // REQUESTED string (m_pendingModes.insert(slice, mode)) and releases it
    // only on an observation comparing EQUAL to it; checkSlice() refuses every
    // further Mode AND Filter intent on that slice with "request.conflict"
    // while the entry stands, and the only other things that clear it are a
    // disconnect, a backend rebuild and the slice's destruction. Now that
    // Hl2Backend::setSliceMode canonicalises, a requested alias would be
    // published back under its canonical spelling, the entry would never
    // release, and the slice would be wedged permanently -- the fault this
    // whole declaration exists to remove, arriving by the other door. An alias
    // on this list stopped being a harmless extra entry the moment the backend
    // started rewriting one.
    for (const QString& declared : hl2Caps.receiveModeControl->modes) {
        check(hl2::canonicalOfferedMode(declared) == declared,
            qPrintable(QStringLiteral("declared mode %1 is its own canonical spelling -- "
                                      "setSliceMode will publish back exactly what was asked for")
                           .arg(declared)));
    }
    // AND THE BACKEND TREATS EACH AS A REAL MODE, not as a string it shrugs at.
    // Hl2Backend::setSliceMode adopts defaultPassbandForMode() on every mode
    // CHANGE, so the published window says whether that table has an entry for
    // the mode or whether it landed on the function's own {150, 3000} fallback
    // -- and a mode with no entry of its own is a mode nobody wrote a passband
    // for. The numbers are READ BACK through the seam rather than re-typed from
    // a table: defaultPassbandForMode() is file-local to Hl2Backend.cpp, so the
    // published SliceDelta is the only production surface that can be asked.
    //
    // PRECISELY WHAT IS AND IS NOT OBSERVED HERE. This reaches
    // defaultPassbandForMode() and NOT modeFromString(): the WDSP mode
    // enumerator goes to the DSP object, which a pre-connect backend does not
    // have, so it never appears in a SliceDelta. Delete DSB from
    // modeFromString() and it would silently demodulate as USB while every
    // assertion below still passed. observed.mode is likewise only a
    // round-trip -- setSliceMode stores the string it was given -- so it is a
    // sanity check, not evidence about the detector.
    hl2.setSliceMode(0, QStringLiteral("DSB"));
    check(observed.mode == QStringLiteral("DSB")
        && observed.filterLow == -3000 && observed.filterHigh == 3000,
        "HL2 DSB is carrier-straddling, not the USB fallback window");
    hl2.setSliceMode(0, QStringLiteral("CWL"));
    check(observed.mode == QStringLiteral("CWL")
        && observed.filterLow == -250 && observed.filterHigh == 250,
        "HL2 CWL is the 500 Hz carrier-centred CW window");
    // THE ALIAS DOES NOT REACH THE SLICE. setSliceMode() runs
    // hl2::canonicalOfferedMode() now, so the "CWU" spelling -- which
    // publishedModeStrings() deliberately does not carry, and which the mode
    // combo therefore cannot display -- is collapsed onto "CW" before it is
    // stored and published. Before this it was stored verbatim, and the combo
    // fell to index 0 ("LSB") while the receiver really was in CW.
    //
    // The passband is asserted in the same breath: the collapse must rename the
    // mode AND land on the CW window -- -250/250 is the CW branch of
    // defaultPassbandForMode(), the same one "CWU" reached before.
    //
    // THE DSB SET BELOW IS LOAD-BEARING AND MUST NOT BE TIDIED AWAY AS A
    // REDUNDANT WRITE. emitSliceState() publishes a full SNAPSHOT of the
    // receiver (d.filterLow = r->filterLowHz, unconditionally), not a diff of
    // what changed, so every assertion in this block reads the window the
    // receiver is holding RIGHT NOW. CWL and CWU share one
    // defaultPassbandForMode() entry, so entering CWU straight out of CWL left
    // the passband clause below unable to fail the way its label claims: a
    // setSliceMode() that stopped adopting a passband while still applying the
    // rename would have left CWL's -250/250 standing and the line would have
    // stayed green. Re-entering from DSB's -3000/3000 is what gives it teeth --
    // the window now has to MOVE for the check to pass. Raised in review on
    // PR #5879; the CWL leg above already had this property because it follows
    // DSB, and this restores it for the CWU leg.
    hl2.setSliceMode(0, QStringLiteral("DSB"));
    hl2.setSliceMode(0, QStringLiteral("CWU"));
    check(observed.mode == QStringLiteral("CW")
        && observed.filterLow == -250 && observed.filterHigh == 250,
        "HL2 collapses the CWU spelling onto CW -- the alias never reaches a slice");
    check(hl2::publishedModeStrings().contains(observed.mode.value_or(QString())),
        "and what a slice ends up holding is a spelling the mode menu can display");
    // NFM is the other pair this can reach, and the one Hl2ModeVocabulary.h
    // wrote the reconciliation argument about in the first place.
    hl2.setSliceMode(0, QStringLiteral("NFM"));
    check(observed.mode == QStringLiteral("FM"),
        "and NFM collapses onto FM for the same reason, at the same seam");
    // A string the vocabulary does not know is passed through UNTOUCHED, not
    // merely upper-cased: isKnownModeString() gates the collapse exactly as
    // applyRestoredState() gates it, so nothing outside the three alias pairs
    // changes shape here.
    hl2.setSliceMode(0, QStringLiteral("RADE"));
    check(observed.mode == QStringLiteral("RADE"),
        "an unknown mode string is left alone, not normalised into something else");
    hl2.setSliceMode(0, QStringLiteral("USB"));
    // WHAT THIS CANNOT SEE, stated rather than implied: that CWL and CWU select
    // DIFFERENT detectors. They share one passband entry by design (the pitch
    // lives in the BFO, cwBfoOffsetHz), so the sideband difference is invisible
    // in a SliceDelta. Distinguishing them needs the WDSP channel, which means
    // a DSP build and therefore is not socket-free.
    anan::AnanBackend anan;
    const auto ananCaps = anan.capabilities();
    check(!ananCaps.receiveModeControl && !ananCaps.receiveFilterControl && !ananCaps.receiveAudioControl
        && !ananCaps.receivePanCenterControl && ananCaps.receivePanBandwidthControl,
        "ANAN exposes qualified bandwidth only, not center that implicitly retunes");
    icom::IcomCivBackend icom;
    const auto icomCaps = icom.capabilities();
    check(!icomCaps.receiveModeControl && !icomCaps.receiveFilterControl && !icomCaps.receiveAudioControl
        && !icomCaps.receivePanCenterControl && !icomCaps.receivePanBandwidthControl,
        "Icom profile-dependent receive contracts remain unavailable");
#ifdef AETHER_BACKEND_RTL
    rtl::RtlSdrBackend rtl;
    const auto rtlCaps = rtl.capabilities();
    check(rtlCaps.receiveModeControl && rtlCaps.receiveFilterControl && rtlCaps.receiveAudioControl
        && rtlCaps.receivePanCenterControl && rtlCaps.receivePanBandwidthControl
        && rtlCaps.panSpanModel && !rtlCaps.panSpanModel->followsSampleRate,
        "RTL exposes independent display center/span alongside implemented FM filter and mixer controls");
    check(rtlCaps.receiveFilterControl && rtlCaps.receiveFilterControl->modes.size() == 2
        && rtlCaps.receiveFilterControl->modes[0].mode == QStringLiteral("FM")
        && rtlCaps.receiveFilterControl->modes[1].mode == QStringLiteral("FMN")
        && rtlCaps.receiveFilterControl->modes[0].minimumLowHz == -21600
        && rtlCaps.receiveFilterControl->modes[1].maximumHighHz == 21600,
        "RTL adjustable filter qualification excludes legacy WFM");
    check(rtlCaps.receiveSquelchModel
        && rtlCaps.receiveSquelchModel->modes == QStringList{"FM", "FMN"}
        && rtlCaps.receiveSquelchModel->referenceDb == -120
        && rtlCaps.receiveSquelchModel->stepDb == 1.2
        && rtlCaps.receiveSquelchModel->unit == QStringLiteral("dBFS/bin"),
        "RTL desktop squelch declares the implemented detector's modes and units");
#endif
}

// THE STRANDING ITSELF, driven through the target that does the stranding.
//
// productionCapabilityContracts() above reads the HL2's receiveModeControl
// list back and checks that four strings are in it. That assertion cannot tell
// a CONSUMED declaration from an ignored one: strip the
// `modes.contains(*observed.mode)` term out of
// ModelReceiveControlTarget::checkSlice and every line of it stays green while
// the refusal it exists to prevent disappears. This target is the other half.
// It installs the HL2's OWN declared list on the fixture backend, reports an
// observed mode, and asks the real ControlService for a mode change. What is
// asserted is the OUTCOME the control plane returns, never agreement between a
// list and itself.
//
// BOTH READINGS OF THE LIST ARE EXERCISED, because they are different failures
// and only one of them is the self-latch this change is about:
//
//   * the OBSERVED mode, read by checkSlice() -- which is the first statement
//     of setMode(). Refusing it is "capability.unavailable" and it is the
//     stranding: NO requested mode gets the slice back out, including a listed
//     one.
//   * the REQUESTED mode, read by setMode() itself. Refusing it is
//     "request.out_of_range" and it declines one intent and nothing more.
//
// The response CODE is compared, not merely the presence of a result, so a
// refusal arriving for some unrelated reason cannot read as the refusal being
// asserted.
//
// Only receiveModeControl is transplanted, deliberately: the rest of the
// fixture's caps (canTransmit=false, the filter/audio/pan declarations) are
// what make it socket-free and controllable, and the subject here is one list.
//
// WHAT THE NEGATIVE CONTROLS USE AND WHY, AND WHY IT IS STILL DRM. "DRM"
// stands for "a mode this list does not carry". It was chosen when FM was one
// of five excluded modes, precisely so that this target took no position on
// the FM question that was then open; FM has since been declared, on the
// separate ground that the mode MENU offers it and an undeclared menu entry
// strands the slice. DRM is unaffected by that ruling and is still the right
// choice: it is not on publishedModeStrings(), so no menu can produce it, and
// there is no DRM decoder in this tree at all -- the least contestable of the
// three remaining exclusions and the one least likely to move. WBFM and WFM
// would serve as well; DRM is kept so the negative controls read the same
// before and after, and so a reader comparing revisions sees the mutants
// measured against an unchanged control.
//
// NOT THE ONLY GUARD ON EITHER TERM, and saying so is cheaper than letting a
// reviewer find it: modeAndFilterOrdering() above already requests "RADE"
// expecting "request.out_of_range", and reports a slice in "RADE" expecting
// "capability.unavailable", so neutralising either read turns one of its lines
// red too. What it does NOT do is speak about the HL2's own declared
// list -- it uses the fixture's synthetic {USB, LSB, AM} -- so it would go on
// passing if this declaration were narrowed instead. The two are different
// questions and both are wanted.
//
// WHAT THIS STILL CANNOT SEE. The fixture's Backend records intents; it does
// not demodulate. So this shows that the declaration is READ and OBEYED by the
// production control target, not that the radio serves the mode -- that claim
// rests on modeFromString() and defaultPassbandForMode(), and on nothing
// measured on hardware.
void hl2DeclaredModesDriveTheControlPlane()
{
    hl2::Hl2Backend hl2;
    const auto hl2Caps = hl2.capabilities();
    const auto outcome = [&](const QString& observedMode, const QString& requestedMode) {
        Fixture f;
        f.backend->caps.receiveModeControl = hl2Caps.receiveModeControl;
        f.report(observedMode, -2900, -100);
        QJsonObject values = f.params(ReceiveOperation::Mode);
        values.insert(QStringLiteral("mode"), requestedMode);
        const QJsonObject response = f.send(ReceiveOperation::Mode, values);
        return response.contains(QStringLiteral("result")) ? QStringLiteral("result") : error(response);
    };
    const auto expect = [&](const char* observedMode, const char* requestedMode,
                            const char* expected, const char* label) {
        const QString actual = outcome(QString::fromLatin1(observedMode), QString::fromLatin1(requestedMode));
        if (actual != QString::fromLatin1(expected)) {
            std::printf("observed %s, requested %s: expected %s, got %s\n",
                        observedMode, requestedMode, expected, qPrintable(actual));
        }
        check(actual == QString::fromLatin1(expected), label);
    };
    // The observed-mode read: a slice sitting in each newly declared mode can
    // still be steered. "LSB" is the requested mode and was on this list in
    // every version of the declaration, so a refusal here is always checkSlice
    // refusing the OBSERVED mode.
    expect("USB", "LSB", "result",
           "positive control: a slice observed in a long-listed mode is steerable");
    expect("DSB", "LSB", "result",
           "a slice observed in DSB is steerable out of it -- the HL2 declares DSB");
    expect("CWL", "LSB", "result",
           "a slice observed in CWL is steerable out of it -- the HL2 declares CWL");
    // FM IS THE ONE THE MENU COULD ACTUALLY PRODUCE. publishedModeStrings()
    // carries "FM", so an operator can pick it out of the combo; before this
    // declaration the set difference between the menu and receiveModeControl
    // was exactly {FM}, and picking it stranded the slice with no way back.
    // This line is that stranding, driven through the target that did it.
    expect("FM", "LSB", "result",
           "a slice observed in FM is steerable out of it -- the menu can put it there");
    // The requested-mode read: each newly declared mode can also be asked for,
    // from a slice whose observed mode was never in question.
    expect("USB", "DSB", "result", "and DSB can be ASKED for, not only sat in");
    expect("USB", "CWL", "result", "and CWL can be asked for");
    // THE REQUEST SIDE OF THE FM DECLARATION, said out loud because it is what
    // the change widens: slice.setMode mode="FM" is accepted here where it was
    // refused "request.out_of_range". Keying is a different list --
    // receiveOnlyModes -- and hl2_fm_controls_declaration_test pins that FM
    // and NFM are still on it.
    expect("USB", "FM", "result", "and FM can be ASKED for, not only sat in");
    // THE ALIAS SPELLINGS ARE REFUSED, and that is the intended shape rather
    // than a gap. "CWU" and "NFM" are not on this list, so a request for one
    // is declined "request.out_of_range" -- which is honest, because
    // Hl2Backend::setSliceMode would rewrite them and the control target's
    // pending-mode entry (keyed on the REQUESTED string) would then never
    // release, wedging every later Mode and Filter intent on the slice with
    // "request.conflict". Refusing one intent beats wedging the slice.
    expect("USB", "CWU", "request.out_of_range",
           "the CWU spelling is declined, not silently rewritten under the client");
    expect("USB", "NFM", "request.out_of_range", "and the NFM spelling likewise");
    // AND NO SLICE CAN BE OBSERVED IN ONE, so refusing them costs no
    // steerability. Every writer of Hl2Backend's Receiver::mode is
    // canonicalising now -- setSliceMode() and applyRestoredState() are the
    // only two, and productionCapabilityContracts() above measures the first
    // through a SliceDelta. This line records what the refusal WOULD be if
    // that ever stopped being true, so the day a new writer reopens the route
    // the reader can see exactly which assertion changes meaning.
    expect("CWU", "LSB", "capability.unavailable",
           "an alias observation would latch -- unreachable today, and pinned so it stays so");
    // The two refusals, which are what prove the list is read at all. Remove
    // either term from ModelReceiveControlTarget and the matching line here
    // turns red while every membership assertion above stays green.
    expect("DRM", "LSB", "capability.unavailable",
           "negative control: an UNDECLARED observed mode latches the slice shut");
    expect("USB", "DRM", "request.out_of_range",
           "negative control: an UNDECLARED requested mode is declined, slice unharmed");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("control-receive"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    schemaAndAuthorization();
    dispatchAndReadback();
    modeAndFilterOrdering();
    safetyAndLifetime();
    delayedIdleCannotAuthorizeReceive();
    rangesAndBudget();
    productionCapabilityContracts();
    hl2DeclaredModesDriveTheControlPlane();
    return failures == 0 ? 0 : 1;
}
