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
#include "core/backends/rtl/RtlReceivePipeline.h"
#include "core/backends/rtl/RtlSdrBackend.h"
#endif

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <cstdio>
#include <limits>

namespace AetherSDR::hl2 {
struct Hl2CwPitchReportTestAccess {
    static void setConnected(Hl2Backend& backend, bool connected) { backend.m_connected = connected; }
};
} // namespace AetherSDR::hl2

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
    SliceFilterRequest::Origin filterOrigin{SliceFilterRequest::Origin::ModeNormalization};
    mutable int reads{0};
    ReceiveControlPolicy policy{ReceiveControlPolicy::Optimistic};
    RadioCapabilities capabilities() const override { ++reads; return caps; }
    ReceiveControlPolicy receiveControlPolicy() const override { return policy; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double hz) override { record("frequency", {{"hz", hz}}); }
    void setSliceMode(int id, const QString& mode) override { record("mode", {{"slice", id}, {"mode", mode}}); }
    void setSliceFilter(int id, int low, int high) override { record("filter", {{"slice", id}, {"low", low}, {"high", high}}); }
    void requestSliceFilter(int id, const SliceFilterRequest& request) override
    {
        filterOrigin = request.origin;
        IRadioBackend::requestSliceFilter(id, request);
    }
    void setSliceAudioGain(int id, int gain) override { record("gain", {{"slice", id}, {"gain", gain}}); }
    void setSliceAudioMute(int id, bool muted) override { record("mute", {{"slice", id}, {"muted", muted}}); }
    void setSliceAgc(int id, const QString& mode, int threshold) override
    {
        record("agc", {{"slice", id}, {"mode", mode}, {"threshold", threshold}});
    }
    ReceiveDispatch requestSliceDsp(int id, const SliceDspRequest& request) override
    {
        record("dsp", {{"slice", id}, {"feature", static_cast<int>(request.feature)},
                       {"enabled", request.enabled}, {"level", request.level}});
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestSliceControl(int id, const SliceControlRequest& request) override
    {
        if (request.field != SliceControlRequest::Field::Rit) {
            return IRadioBackend::requestSliceControl(id, request);
        }
        const IncrementalTuning tuning = std::get<IncrementalTuning>(request.value);
        record("rit", {{"slice", id}, {"enabled", tuning.enabled}, {"offsetHz", tuning.hz}});
        return ritDispatch;
    }
    ReceiveDispatch ritDispatch{ReceiveDispatch::Dispatched};
    void setPanRfGain(const QString& id, int gainDb) override { panId = id; record("rfGain", {{"gainDb", gainDb}}); }
    ReceiveDispatch requestRadioSpeakerAudio(bool on) override
    {
        record("speaker", {{"enabled", on}});
        return ReceiveDispatch::Dispatched;
    }
    void setPanCenter(const QString& id, double hz, PanCenterIntent) override { panId = id; record("center", {{"hz", hz}}); }
    void setPanBandwidth(const QString& id, double hz) override { panId = id; record("bandwidth", {{"hz", hz}}); }
    void setCwPitch(int hz) override { record("pitch", {{"hz", hz}}); }
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
    const ResourceAddress sessionAddress{QStringLiteral("radioSession"), {}, QStringLiteral("radio-1")};

    explicit Fixture(ReceiveControlPolicy policy = ReceiveControlPolicy::Optimistic)
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        backend->policy = policy;
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
        backend->caps.receiveCwPitchControl = ReceiveCwPitchControl{SliceFrequencyControl::Authority::Engine, 100, 6000, 10};
        ReceiveDspControl dsp;
        dsp.modes = {"USB", "LSB", "AM"};
        dsp.features = {ReceiveDspControl::Feature::Agc, ReceiveDspControl::Feature::Nb,
                        ReceiveDspControl::Feature::Nr, ReceiveDspControl::Feature::Rit};
        dsp.authority = Authority::Engine;
        dsp.maximumRitOffsetHz = 9999;
        backend->caps.receiveDspControl = dsp;
        backend->caps.receivePanRfGainControl = ReceivePanRfGainControl{Authority::Engine, -31, 0, 1};
        backend->caps.radioSpeakerControl = RadioSpeakerControl{Authority::Engine};
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
        SliceDelta dspReport; dspReport.agcThreshold = 50; dspReport.nb = false; dspReport.nr = false;
        dspReport.ritOn = false; dspReport.ritFreq = 0;
        radio.slice(0)->applyChanges(dspReport);
        backend->panCenterBandwidthChanged(kBackendPan, 14.225, .192);
        // setBackendForTest() skips setupBackend(), which wires panRfGainChanged
        // to the observation, so the report is recorded the way that wiring does.
        radio.panadapter(pan)->recordRfGainObservation(0);
        backend->radioSpeakerAudioReported(false);
        reportPitch(600);
        adapter.publishAll();
        hello(controller);
    }

    void hello(ControlSession& session)
    {
        check(call(service, session, "hello", {{"versions", QJsonArray{1}}}).contains("result"), "session negotiated");
    }
    void reportPitch(int hz)
    {
        TransmitDelta delta; delta.cwPitch = hz;
        radio.transmitModel().applyChanges(delta);
    }
    void report(const QString& mode, int low, int high)
    {
        SliceDelta delta; delta.mode = mode; delta.filterLow = low; delta.filterHigh = high;
        radio.slice(0)->applyChanges(delta);
    }
    QJsonObject params(ReceiveOperation op) const
    {
        const bool isPan = op == ReceiveOperation::PanCenter || op == ReceiveOperation::PanBandwidth
            || op == ReceiveOperation::PanRfGain;
        const bool isSession = op == ReceiveOperation::CwPitch || op == ReceiveOperation::RadioSpeaker;
        const auto snapshot = store.get(isSession ? sessionAddress : isPan ? panAddress : sliceAddress);
        QJsonObject values{{"radioSession", "radio-1"},
            {"expectedRevision", static_cast<qint64>(snapshot ? snapshot->revision : 0)}};
        if (!isSession) {
            values.insert(isPan ? "panadapter" : "slice", isPan ? pan : "0");
        }
        switch (op) {
        case ReceiveOperation::Mode: values.insert("mode", "LSB"); break;
        case ReceiveOperation::Filter: values.insert("lowHz", 200); values.insert("highHz", 2900); break;
        case ReceiveOperation::AudioGain: values.insert("gain", 35); break;
        case ReceiveOperation::AudioMute: values.insert("muted", true); break;
        case ReceiveOperation::PanCenter: values.insert("hz", 14230000); break;
        case ReceiveOperation::PanBandwidth: values.insert("hz", 96000); break;
        case ReceiveOperation::CwPitch: values.insert("hz", 700); break;
        case ReceiveOperation::AgcThreshold: values.insert("threshold", 65); break;
        case ReceiveOperation::NoiseBlanker:
        case ReceiveOperation::NoiseReduction: values.insert("enabled", true); break;
        case ReceiveOperation::Rit: values.insert("enabled", true); values.insert("offsetHz", -350); break;
        case ReceiveOperation::PanRfGain: values.insert("gainDb", -12); break;
        case ReceiveOperation::RadioSpeaker: values.insert("enabled", true); break;
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
        if (good.contains("slice") || good.contains("panadapter")) {
            bad = good; bad.insert(good.contains("slice") ? "slice" : "panadapter", "missing");
            f.reject(op, bad, good.contains("slice") ? "request.invalid_params" : "resource.not_found");
        } else {
            bad = good; bad.insert("slice", "0");
            f.reject(op, bad, "request.invalid_params");
        }
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
    for (ReceiveOperation op : {ReceiveOperation::Filter, ReceiveOperation::AudioGain, ReceiveOperation::PanCenter,
                                ReceiveOperation::PanBandwidth, ReceiveOperation::CwPitch}) {
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
        const bool pan = op == ReceiveOperation::PanCenter || op == ReceiveOperation::PanBandwidth
            || op == ReceiveOperation::PanRfGain;
        const ResourceAddress address = op == ReceiveOperation::CwPitch || op == ReceiveOperation::RadioSpeaker ? f.sessionAddress
            : pan ? f.panAddress : f.sliceAddress;
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
    const quint64 epoch = f.radio.slice(0)->userFilterEpoch();
    check(f.send(ReceiveOperation::Filter, filter).contains("result"), "new-mode passband dispatches after full observation");
    check(f.backend->filterOrigin == SliceFilterRequest::Origin::Operator
              && f.radio.slice(0)->userFilterEpoch() == epoch + 1
              && f.radio.slice(0)->filterLow() == -2800,
          "daemon filter is explicit operator intent with one epoch advance and no optimistic passband");
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
    // The reclaim after a reconnect publishes the pan again (ensureOwnedPanadapter).
    f.radio.panadapter(f.pan)->setLifecyclePublished(true);
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
    hl2::Hl2Backend::setHostOffersTransmit(false);
    {
        hl2::Hl2Backend headless;
        check(!headless.capabilities().canTransmit,
            "an HL2 in a host without an HL2 transmit path (aetherd) declares itself receive only");
    }
    hl2::Hl2Backend::setHostOffersTransmit(true);
    // The HL2's adjustable filters: every mode whose cuts reach the NBP stage
    // and are read back. WBFM/WFM/DRM never appear (not receive-controllable).
    QStringList hl2FilterModes;
    const auto hl2Filter = [&](const QString& mode) -> const ReceiveFilterMode* {
        for (const ReceiveFilterMode& filter : hl2Caps.receiveFilterControl->modes) {
            if (filter.mode == mode) { return &filter; }
        }
        return nullptr;
    };
    for (const ReceiveFilterMode& filter : hl2Caps.receiveFilterControl->modes) {
        hl2FilterModes.append(filter.mode);
    }
    hl2FilterModes.sort();
    check(hl2FilterModes == QStringList{"AM", "CW", "CWL", "DIGL", "DIGU", "DSB", "FM", "LSB", "SAM", "USB"},
        "HL2 declares adjustable filters for exactly its sideband, data, AM, DSB, CW and FM modes");
    bool filterModesRequestable = true;
    for (const QString& mode : hl2FilterModes) {
        filterModesRequestable &= hl2Caps.receiveModeControl->modes.contains(mode);
    }
    check(filterModesRequestable, "every HL2 filter mode is also a requestable receive mode");
    const ReceiveFilterMode* hl2Dsb = hl2Filter(QStringLiteral("DSB"));
    const ReceiveFilterMode* hl2Fm = hl2Filter(QStringLiteral("FM"));
    const ReceiveFilterMode* hl2Cw = hl2Filter(QStringLiteral("CW"));
    const ReceiveFilterMode* hl2Cwl = hl2Filter(QStringLiteral("CWL"));
    check(hl2Dsb && hl2Fm && hl2Cw && hl2Cwl, "HL2 DSB, FM, CW and CWL filter records exist");
    if (hl2Dsb && hl2Fm && hl2Cw && hl2Cwl) {
        // Carrier-straddling modes: the carrier stays inside every accepted
        // passband, so a low cut is always negative and a high cut positive.
        bool straddles = true;
        for (const ReceiveFilterMode* range : {hl2Dsb, hl2Fm, hl2Cw, hl2Cwl}) {
            straddles &= range->maximumLowHz < 0 && range->minimumHighHz > 0;
        }
        check(straddles, "HL2 DSB/FM/CW/CWL filters always straddle the carrier");
        // The default passbands the backend adopts on a mode change fit.
        check(hl2Dsb->minimumLowHz <= -3000 && hl2Dsb->maximumHighHz >= 3000
                && hl2Fm->minimumLowHz <= -8000 && hl2Fm->maximumHighHz >= 8000
                && hl2Cw->minimumLowHz <= -250 && hl2Cw->maximumHighHz >= 250
                && hl2Cwl->minimumLowHz <= -250 && hl2Cwl->maximumHighHz >= 250,
            "HL2 default DSB, FM and CW passbands lie inside their declared ranges");
        // CW cuts stop at 6000 Hz: the BFO (pitch up to 6000 Hz) is added on top.
        check(hl2Cw->minimumLowHz == -6000 && hl2Cw->maximumHighHz == 6000
                && hl2Cwl->minimumLowHz == -6000 && hl2Cwl->maximumHighHz == 6000,
            "HL2 CW and CWL cuts leave room for the maximum CW pitch");
    }
    check(!hl2Filter(QStringLiteral("WBFM")) && !hl2Filter(QStringLiteral("WFM"))
            && !hl2Filter(QStringLiteral("DRM")) && !hl2Filter(QStringLiteral("NFM"))
            && !hl2Filter(QStringLiteral("CWU")),
        "HL2 offers no filter for modes it cannot be asked to enter or that are aliases");
    QStringList flexFilterModes;
    for (const ReceiveFilterMode& filter : flexCaps.receiveFilterControl->modes) {
        flexFilterModes.append(filter.mode);
    }
    flexFilterModes.sort();
    check(flexFilterModes == QStringList{"AM", "DIGL", "DIGU", "DSB", "LSB", "SAM", "USB"},
        "Flex filter modes are unchanged by the HL2 declaration");
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
    // A string the vocabulary does not know is neither normalised nor stored:
    // it is refused and the slice re-published as it was, FM from the leg
    // above (#5580). "RADE" never reaches a backend in production (RxApplet
    // and VfoWidget return before setMode()), so it is a safe unknown.
    hl2.setSliceMode(0, QStringLiteral("RADE"));
    check(observed.mode == QStringLiteral("FM"),
        "an unknown mode string is refused -- neither stored nor normalised; "
        "the slice stays in FM");
    hl2.setSliceMode(0, QStringLiteral("USB"));
    // WHAT THIS CANNOT SEE, stated rather than implied: that CWL and CWU select
    // DIFFERENT detectors. They share one passband entry by design (the pitch
    // lives in the BFO, cwBfoOffsetHz), so the sideband difference is invisible
    // in a SliceDelta. Distinguishing them needs the WDSP channel, which means
    // a DSP build and therefore is not socket-free.
    anan::AnanBackend anan;
    const auto ananCaps = anan.capabilities();
    check(!ananCaps.receivePanCenterControl && ananCaps.receivePanBandwidthControl,
        "ANAN exposes qualified bandwidth only, not center that implicitly retunes");
    // ANAN demodulates in the same host WDSP as the HL2, and Protocol 2 carries
    // no mode, so it takes the HL2's mode contract: the menu and the control
    // plane agree both ways, and every declared mode is its own canonical
    // spelling (the wedge pinned above for the HL2).
    check(ananCaps.receiveModeControl
        && ananCaps.receiveModeControl->authority == SliceFrequencyControl::Authority::Engine
        && ananCaps.receiveModeControl->modes == anan::AnanBackend::publishedModeStrings(),
        "ANAN declares exactly the modes its menu offers -- no stranding, no wedge");
    for (const QString& declared : ananCaps.receiveModeControl->modes) {
        check(anan::AnanBackend::canonicalOfferedMode(declared) == declared
            && anan::AnanBackend::isKnownModeString(declared),
            qPrintable(QStringLiteral("ANAN declared mode %1 is accepted and its own canonical spelling")
                           .arg(declared)));
    }
    QStringList ananFilterModes;
    for (const auto& filter : ananCaps.receiveFilterControl
             ? ananCaps.receiveFilterControl->modes : QList<ReceiveFilterMode>{}) {
        ananFilterModes.append(filter.mode);
        // The CW BFO is clamped to 6000 Hz, so a CW cut at the limit keeps the
        // demodulator's edge inside the +-12000 Hz the other modes stop at.
        if (filter.mode == QLatin1String("CW") || filter.mode == QLatin1String("CWL")) {
            check(filter.minimumLowHz + -6000 >= -12000 && filter.maximumHighHz + 6000 <= 12000,
                "ANAN CW cuts plus the clamped BFO stay inside the DSP passband");
        }
    }
    QStringList ananModesSorted = ananCaps.receiveModeControl->modes;
    ananModesSorted.sort();
    ananFilterModes.sort();
    check(ananFilterModes == ananModesSorted,
        "ANAN declares a filter range for every steerable mode, and only those");
    SliceDelta ananObserved;
    QObject::connect(&anan, &IRadioBackend::sliceChanged, &anan,
        [&](int, const SliceDelta& delta) { ananObserved = delta; });
    anan.setSliceMode(0, QStringLiteral("DSB"));
    check(ananObserved.mode == QStringLiteral("DSB")
        && ananObserved.filterLow == -3000 && ananObserved.filterHigh == 3000
        && ananObserved.modeList == anan::AnanBackend::publishedModeStrings(),
        "ANAN DSB lands on its carrier-straddling window and publishes the menu list");
    // Re-entered from DSB so the window must MOVE for the check to pass.
    anan.setSliceMode(0, QStringLiteral("CWU"));
    check(ananObserved.mode == QStringLiteral("CW")
        && ananObserved.filterLow == -250 && ananObserved.filterHigh == 250,
        "ANAN collapses CWU onto CW -- the alias never reaches a slice");
    anan.setSliceMode(0, QStringLiteral("RTTY"));
    check(ananObserved.mode == QStringLiteral("DIGU"),
        "ANAN collapses RTTY onto DIGU, the detector it already ran");
    anan.setSliceMode(0, QStringLiteral("NFM"));
    check(ananObserved.mode == QStringLiteral("FM"), "and NFM onto FM");
    anan.setSliceMode(0, QStringLiteral("DFM"));
    check(ananObserved.mode == QStringLiteral("FM"),
        "ANAN refuses DFM, which it would have demodulated as USB; the slice stays in FM");
    // Audio IS declared now: the backend applies gain and mute to the block it
    // publishes, which is what the record promises. Before that it was absent and
    // correctly so -- the three setters were IRadioBackend's no-ops, so declaring
    // it would have offered a control that moved and did nothing.
    check(ananCaps.receiveAudioControl
        && ananCaps.receiveAudioControl->authority
               == SliceFrequencyControl::Authority::Engine,
        "ANAN declares Engine-authority receive audio, because it applies it");
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
    // WFM joins the adjustable filters only through the qualified recipe (the
    // build gate #5973 turned on); legacy WFM never does.
    const bool qualifiedWfm = rtl::RtlReceivePipeline::kQualifiedWfmEnabled;
    const auto& rtlFilters = rtlCaps.receiveFilterControl;
    QStringList rtlFilterModes;
    if (rtlFilters) {
        for (const auto& filter : rtlFilters->modes) { rtlFilterModes.append(filter.mode); }
    }
    QStringList expectedFilterModes{"AM", "SAM", "FM", "FMN", "USB", "LSB", "CW", "CWR"};
    if (qualifiedWfm) { expectedFilterModes.append(QStringLiteral("WFM")); }
    const auto rtlFilter = [&](const QString& mode) -> const ReceiveFilterMode* {
        if (!rtlFilters) { return nullptr; }
        for (const auto& filter : rtlFilters->modes) {
            if (filter.mode == mode) { return &filter; }
        }
        return nullptr;
    };
    const ReceiveFilterMode* fm = rtlFilter(QStringLiteral("FM"));
    const ReceiveFilterMode* fmn = rtlFilter(QStringLiteral("FMN"));
    check(rtlFilterModes == expectedFilterModes
        && fm && fm->minimumLowHz == -21600
        && fmn && fmn->maximumHighHz == 21600,
        "RTL adjustable filters are AM/SAM, FM/FMN, SSB and CW, plus WFM only when qualified");
    const ReceiveFilterMode* wfm = rtlFilter(QStringLiteral("WFM"));
    check(!qualifiedWfm
        || (wfm && wfm->minimumLowHz == -100000
            && wfm->maximumHighHz == 100000
            && wfm->minimumWidthHz == 30000
            && wfm->maximumWidthHz == 200000
            && rtlCaps.broadcastFmReceive),
        "qualified WFM declares its own filter range and the broadcast-FM controls");
    check(qualifiedWfm || !rtlCaps.broadcastFmReceive,
        "without qualified WFM, RTL offers no broadcast-FM controls");
    check(rtlCaps.squelchLevelScale && rtlCaps.squelchLevelScale->modesExclusive
        && rtlCaps.squelchLevelScale->automaticInEngine && !rtlCaps.squelchLevelScale->spectrumComparable
        && rtlCaps.squelchLevelScale->modes == QStringList{"FM", "FMN"}
        && rtlCaps.squelchLevelScale->offsetDb == -120
        && rtlCaps.squelchLevelScale->dbPerStep == 1.2
        && rtlCaps.squelchLevelScale->unit == QStringLiteral("dBFS/2048-bin"),
        "RTL desktop squelch declares the implemented detector's modes and units");
#endif
}

void hl2DeclaredFiltersDriveTheControlPlane()
{
    hl2::Hl2Backend hl2;
    const RadioCapabilities caps = hl2.capabilities();
    const auto expect = [&](const char* mode, int low, int high, const char* expected) {
        Fixture f;
        f.backend->caps.receiveModeControl = caps.receiveModeControl;
        f.backend->caps.receiveFilterControl = caps.receiveFilterControl;
        f.report(QString::fromLatin1(mode), -250, 250);
        QJsonObject values = f.params(ReceiveOperation::Filter);
        values.insert(QStringLiteral("lowHz"), low);
        values.insert(QStringLiteral("highHz"), high);
        const QJsonObject response = f.send(ReceiveOperation::Filter, values);
        const QString actual = response.contains(QStringLiteral("result"))
            ? QStringLiteral("result") : error(response);
        if (actual != QString::fromLatin1(expected)) {
            std::printf("%s filter %d:%d: expected %s, got %s\n",
                        mode, low, high, expected, qPrintable(actual));
        }
        check(actual == QString::fromLatin1(expected),
              "HL2 production filter record controls daemon admission");
        if (QString::fromLatin1(expected) == QStringLiteral("result")) {
            check(response.value("result").toObject().value("accepted").toBool()
                      && f.backend->intents == 1 && f.backend->method == QStringLiteral("filter")
                      && f.backend->args.value("low").toInt() == low
                      && f.backend->args.value("high").toInt() == high
                      && f.backend->filterOrigin == SliceFilterRequest::Origin::Operator,
                  "accepted HL2 passband dispatches exactly one unchanged operator intent");
        } else {
            check(f.backend->intents == 0, "refused HL2 passband dispatches no backend intent");
        }
        check(f.backend->keys == 0, "HL2 filter admission never dispatches keying");
    };
    expect("CW", -250, 250, "result");
    expect("CWL", -250, 250, "result");
    expect("FM", -8000, 8000, "result");
    expect("DSB", -3000, 3000, "result");
    expect("CW", -6000, 6000, "result");
    expect("CWL", -6000, 6000, "result");
    expect("FM", -12000, 12000, "result");
    expect("DSB", -10, 10, "result");
    expect("CW", -6001, 250, "request.out_of_range");
    expect("CWL", 100, 500, "request.out_of_range");
    expect("FM", -12001, 8000, "request.out_of_range");
    expect("DSB", -5, 5, "request.out_of_range");
    expect("CW", -250, 0, "request.out_of_range");
    expect("CWL", -250, 6001, "request.out_of_range");
    expect("WBFM", -8000, 8000, "capability.unavailable");
    expect("DRM", -3000, 3000, "capability.unavailable");
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
    // receiveOnlyModes -- and hl2_fm_controls_declaration_test pins whether FM
    // and NFM are on it for this build.
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

// radioSession.setCwPitch: one session-wide pitch, admitted from the declared
// record and a backend-reported pitch, never from the model's own value.
void cwPitchControl()
{
    {
        Fixture f;
        const QJsonObject observed = f.store.get(f.sessionAddress)->value
            .value("receiveObservation").toObject().value("cwPitch").toObject();
        check(observed.value("known").toBool() && observed.value("hz").toInt() == 600,
              "the radio session publishes the reported CW pitch");
        const QJsonObject declared = f.store.get(f.sessionAddress)->value.value("capabilities").toObject()
            .value("receiveCwPitchControl").toObject();
        check(declared.value("authority").toString() == QLatin1String("engine")
                  && declared.value("minimumHz").toInt() == 100 && declared.value("maximumHz").toInt() == 6000
                  && declared.value("stepHz").toInt() == 10,
              "the radio session publishes the declared pitch authority, range and step");
        for (int hz : {99, 6001, 705, 0, -600}) {
            auto params = f.params(ReceiveOperation::CwPitch); params.insert("hz", hz);
            f.reject(ReceiveOperation::CwPitch, params, "request.out_of_range");
        }
        for (int hz : {100, 6000, 750}) {
            const int before = f.backend->intents;
            auto params = f.params(ReceiveOperation::CwPitch); params.insert("hz", hz);
            check(f.send(ReceiveOperation::CwPitch, params).contains("result")
                      && f.backend->intents == before + 1 && f.backend->method == "pitch"
                      && f.backend->args.value("hz").toInt() == hz,
                  "a declared pitch on the step dispatches exactly one unchanged pitch intent");
        }
        check(f.radio.transmitModel().cwPitch() == 600,
              "acceptance does not write the model's pitch before the backend reports it");
        const auto stale = f.params(ReceiveOperation::CwPitch);
        f.reportPitch(750);
        check(f.store.get(f.sessionAddress)->value.value("receiveObservation").toObject()
                  .value("cwPitch").toObject().value("hz").toInt() == 750
                  && f.radio.transmitModel().cwPitch() == 750,
              "a pitch report updates the published observation and the model");
        f.reject(ReceiveOperation::CwPitch, stale, "request.conflict");
        check(f.backend->keys == 0, "pitch control never keys");
    }
    {
        Fixture f;
        f.radio.transmitModel().resetState();
        f.radio.transmitModel().setCwPitch(800);
        check(!f.target->available(ReceiveOperation::CwPitch)
                  && !f.service.capabilities(f.controller).value("capabilities").toArray()
                         .contains(QStringLiteral("radioSession.setCwPitch")),
              "negative control: a model-only pitch is not an observation, so pitch is not offered");
        f.reject(ReceiveOperation::CwPitch, f.params(ReceiveOperation::CwPitch), "capability.unavailable");
        f.reportPitch(800);
        check(f.target->available(ReceiveOperation::CwPitch), "a backend pitch report re-enables pitch control");
    }
    {
        Fixture f;
        f.backend->caps.receiveCwPitchControl.reset();
        check(!f.target->available(ReceiveOperation::CwPitch)
                  && !f.service.capabilities(f.controller).value("capabilities").toArray()
                         .contains(QStringLiteral("radioSession.setCwPitch")),
              "negative control: a backend without receiveCwPitchControl is not offered pitch");
        f.reject(ReceiveOperation::CwPitch, f.params(ReceiveOperation::CwPitch), "capability.unavailable");
        check(f.target->available(ReceiveOperation::AudioGain),
              "the other receive controls stay offered without a pitch record");
    }
#ifdef AETHER_BACKEND_RTL
    {
        rtl::RtlSdrBackend rtl;
        const auto pitch = rtl.capabilities().receiveCwPitchControl;
        check(pitch && pitch->authority == SliceFrequencyControl::Authority::Engine
                  && pitch->minimumHz == 100 && pitch->maximumHz == 6000 && pitch->stepHz == 10
                  && rtl.receiveControlPolicy() == ReceiveControlPolicy::Confirmed,
              "RTL declares an engine-owned 100..6000 Hz pitch in 10 Hz steps and confirms it");
    }
#endif
    {
        // A confirmed backend (RTL's shape) is offered the verb on the same terms.
        Fixture f(ReceiveControlPolicy::Confirmed);
        check(f.radio.transmitModel().confirmsReceiveCwPitch()
                  && f.service.capabilities(f.controller).value("capabilities").toArray()
                         .contains(QStringLiteral("radioSession.setCwPitch")),
              "a confirmed backend with the record takes the confirmed route and is offered pitch");
        const int before = f.backend->intents;
        check(f.send(ReceiveOperation::CwPitch, f.params(ReceiveOperation::CwPitch)).contains("result")
                  && f.backend->intents == before + 1 && f.backend->method == "pitch"
                  && f.backend->args.value("hz").toInt() == 700
                  && f.radio.transmitModel().cwPitch() == 600,
              "a confirmed backend gets one pitch intent and the model waits for its report");
        f.reportPitch(700);
        check(f.store.get(f.sessionAddress)->value.value("receiveObservation").toObject()
                  .value("cwPitch").toObject().value("hz").toInt() == 700,
              "the confirmed report updates the published observation");
    }
}

// The production records: HL2 declares the pitch and reports what it adopted;
// Flex, Demo, ANAN and Icom do not declare it.
void hl2CwPitchIsDeclaredAndReported()
{
    FlexBackend flex;
    SimBackend sim;
    anan::AnanBackend anan;
    icom::IcomCivBackend icom;
    check(!flex.capabilities().receiveCwPitchControl && !sim.capabilities().receiveCwPitchControl
              && !anan.capabilities().receiveCwPitchControl && !icom.capabilities().receiveCwPitchControl,
          "Flex, Demo, ANAN and Icom declare no receive CW pitch");
    hl2::Hl2Backend hl2;
    const RadioCapabilities caps = hl2.capabilities();
    check(caps.receiveCwPitchControl
              && caps.receiveCwPitchControl->authority == SliceFrequencyControl::Authority::Engine
              && caps.receiveCwPitchControl->minimumHz == 100
              && caps.receiveCwPitchControl->maximumHz == 6000 && caps.receiveCwPitchControl->stepHz == 10
              && hl2.receiveControlPolicy() == ReceiveControlPolicy::Optimistic,
          "HL2 declares an engine-owned 100..6000 Hz pitch in 10 Hz steps and stays an optimistic backend");
    QList<int> reports;
    QObject::connect(&hl2, &IRadioBackend::transmitChanged, &hl2, [&](const TransmitDelta& delta) {
        if (delta.cwPitch) { reports.append(*delta.cwPitch); }
    });
    hl2.setCwPitch(700);
    check(reports.isEmpty(), "negative control: a disconnected HL2 reports no pitch");
    hl2::Hl2CwPitchReportTestAccess::setConnected(hl2, true);
    hl2.setCwPitch(700);
    hl2.setCwPitch(720);
    hl2.setCwPitch(9000);
    hl2::Hl2CwPitchReportTestAccess::setConnected(hl2, false);
    if (reports != QList<int>{700, 720, 6000}) {
        std::printf("HL2 pitch reports:");
        for (int hz : reports) { std::printf(" %d", hz); }
        std::printf("\n");
    }
    check(reports == QList<int>{700, 720, 6000},
          "a connected HL2 reports the adopted pitch on every call, same value and clamp included");
}
} // namespace

// The G2 front-panel verbs: each one's own range, mode gating and the exact
// typed intent it reaches the backend with.
void panelVerbs()
{
    const auto with = [](QJsonObject values, const char* key, const QJsonValue& value) {
        values.insert(QLatin1String(key), value);
        return values;
    };
    {
        Fixture f;
        f.reject(ReceiveOperation::AgcThreshold, with(f.params(ReceiveOperation::AgcThreshold), "threshold", 101),
                 "request.out_of_range");
        f.reject(ReceiveOperation::AgcThreshold, with(f.params(ReceiveOperation::AgcThreshold), "threshold", -1),
                 "request.out_of_range");
        check(f.send(ReceiveOperation::AgcThreshold, f.params(ReceiveOperation::AgcThreshold)).contains("result")
                  && f.backend->method == "agc" && f.backend->args.value("threshold").toInt() == 65
                  && f.backend->args.value("mode").toString() == f.radio.slice(0)->agcMode(),
              "AGC threshold reaches the backend with the slice's current AGC mode");
    }
    {
        Fixture f;
        f.reject(ReceiveOperation::Rit, with(f.params(ReceiveOperation::Rit), "offsetHz", 10000), "request.out_of_range");
        f.reject(ReceiveOperation::Rit, with(f.params(ReceiveOperation::Rit), "offsetHz", -10000), "request.out_of_range");
        check(f.send(ReceiveOperation::Rit, f.params(ReceiveOperation::Rit)).contains("result")
                  && f.backend->method == "rit" && f.backend->args.value("enabled").toBool()
                  && f.backend->args.value("offsetHz").toInt() == -350,
              "RIT enable and offset reach the backend as ONE intent");
    }
    {
        Fixture f;
        auto off = with(f.params(ReceiveOperation::Rit), "enabled", false);
        off.insert("offsetHz", 15000);
        check(f.send(ReceiveOperation::Rit, off).contains("result")
                  && f.backend->method == "rit" && !f.backend->args.value("enabled").toBool(),
              "turning RIT off is never refused for its offset");
    }
    {
        Fixture f;
        f.backend->ritDispatch = ReceiveDispatch::Unsupported;
        check(error(f.send(ReceiveOperation::Rit, f.params(ReceiveOperation::Rit)))
                  == QStringLiteral("capability.unavailable"),
              "a backend that does not take the RIT intent is a refusal, not a silent accept");
    }
    {
        Fixture f;
        check(f.send(ReceiveOperation::NoiseReduction, f.params(ReceiveOperation::NoiseReduction)).contains("result")
                  && f.backend->method == "dsp"
                  && f.backend->args.value("feature").toInt() == static_cast<int>(SliceDspRequest::Feature::Nr)
                  && f.backend->args.value("enabled").toBool(),
              "noise reduction is a typed Nr DSP intent");
        Fixture g;
        check(g.send(ReceiveOperation::NoiseBlanker, g.params(ReceiveOperation::NoiseBlanker)).contains("result")
                  && g.backend->args.value("feature").toInt() == static_cast<int>(SliceDspRequest::Feature::Nb),
              "the noise blanker is a typed Nb DSP intent");
    }
    {
        Fixture f;
        f.reject(ReceiveOperation::PanRfGain, with(f.params(ReceiveOperation::PanRfGain), "gainDb", 1), "request.out_of_range");
        f.reject(ReceiveOperation::PanRfGain, with(f.params(ReceiveOperation::PanRfGain), "gainDb", -32), "request.out_of_range");
        check(f.send(ReceiveOperation::PanRfGain, f.params(ReceiveOperation::PanRfGain)).contains("result")
                  && f.backend->method == "rfGain" && f.backend->args.value("gainDb").toInt() == -12
                  && f.backend->panId == kBackendPan,
              "RF gain reaches the exact backend pan within the declared dB range");
    }
    {
        // A DSP feature the record does not declare for the observed mode is
        // unavailable, not a guess: DIGU is not in this fixture's DSP modes.
        Fixture f;
        f.backend->caps.receiveModeControl->modes.append(QStringLiteral("DIGU"));
        f.report("DIGU", 150, 3000);
        f.adapter.publishAll();
        for (const ReceiveOperation op : {ReceiveOperation::AgcThreshold, ReceiveOperation::NoiseBlanker,
                                          ReceiveOperation::NoiseReduction, ReceiveOperation::Rit}) {
            f.reject(op, f.params(op), "capability.unavailable");
        }
        // And an Unknown-authority record stays GUI-only.
        Fixture g;
        g.backend->caps.receiveDspControl->authority = SliceFrequencyControl::Authority::Unknown;
        g.reject(ReceiveOperation::AgcThreshold, g.params(ReceiveOperation::AgcThreshold), "capability.unavailable");
        check(!g.target->available(ReceiveOperation::Rit), "an Unknown-authority DSP record offers no headless verb");
    }
}

// The radio's own speaker: session-scoped, reported by the backend, refused until it is.
void radioSpeakerVerb()
{
    {
        Fixture f;
        check(f.send(ReceiveOperation::RadioSpeaker, f.params(ReceiveOperation::RadioSpeaker)).contains("result")
                  && f.backend->method == "speaker" && f.backend->args.value("enabled").toBool(),
              "setRadioSpeaker reaches the backend as one typed intent");
        f.backend->radioSpeakerAudioReported(true);
        const auto obs = f.store.get(f.sessionAddress)->value.value("receiveObservation").toObject()
                             .value("radioSpeaker").toObject();
        check(obs.value("known").toBool() && obs.value("enabled").toBool(),
              "the session resource reports the speaker as the backend adopted it");
    }
    {
        // A new backend forgets the old one's speaker report (onDisconnected() clears it
        // too; the fixture's injected backend never reaches that path).
        Fixture f;
        check(f.radio.reportedRadioSpeaker() == false, "the fixture's backend reported the speaker off");
        f.radio.setBackendForTest(std::make_unique<Backend>(), QStringLiteral("receive-test"));
        check(!f.radio.reportedRadioSpeaker().has_value(),
              "a replaced backend's speaker report is forgotten");
    }
    {
        Fixture f;
        f.backend->caps.radioSpeakerControl.reset();
        f.reject(ReceiveOperation::RadioSpeaker, f.params(ReceiveOperation::RadioSpeaker), "capability.unavailable");
        check(!f.target->available(ReceiveOperation::RadioSpeaker), "no record, no speaker verb");
    }
}

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
    hl2DeclaredFiltersDriveTheControlPlane();
    hl2DeclaredModesDriveTheControlPlane();
    cwPitchControl();
    hl2CwPitchIsDeclaredAndReported();
    panelVerbs();
    radioSpeakerVerb();
    return failures == 0 ? 0 : 1;
}
