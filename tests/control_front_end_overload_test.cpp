// The frontEndOverload daemon resource: offered only where the backend declares
// RadioCapabilities::frontEndOverloadObservation, carried only while connected,
// and never touching another resource's revision. Socket-free: an injected
// backend and the production RadioModel, RadioResourceAdapter and ControlService.
// Nothing here ran on a radio.

#include "TestSettingsProfile.h"
#include "core/control/ControlService.h"
#include "core/control/RadioResourceAdapter.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringList>

#include <array>
#include <cstdio>
#include <memory>

using namespace AetherSDR;
using namespace AetherSDR::control;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

constexpr std::array kLevels{FrontEndLevel::Unobserved, FrontEndLevel::Clean,
    FrontEndLevel::Marginal, FrontEndLevel::Hot, FrontEndLevel::AtFloor};

class Backend final : public IRadioBackend {
public:
    bool connected{false};
    bool declares{false};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities c;
        if (declares) {
            c.frontEndOverloadObservation = RadioCapabilities::FrontEndOverloadObservation{};
        }
        return c;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override
    {
        check(false, "an overload observation must never key");
    }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

QJsonObject call(ControlService& service, ControlSession& session, const QString& method,
                 const QJsonObject& params)
{
    QJsonObject request{{"v", 1}, {"id", "test"}, {"method", method}, {"params", params}};
    if (method != QStringLiteral("hello")) {
        request.insert("sessionId", session.sessionId());
    }
    return service.handle(QJsonDocument(request).toJson(QJsonDocument::Compact), &session).message;
}

struct Fixture {
    RadioModel radio;
    ControlResourceStore store;
    Backend* backend{nullptr};
    std::unique_ptr<RadioResourceAdapter> adapter;
    ControlService service{&store};
    ControlSession client{&store, 262144, SessionAuthorization::Observer};
    const ResourceAddress address{"frontEndOverload", "radio-1", {}};

    explicit Fixture(bool declares)
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        backend->declares = declares;
        radio.setBackendForTest(std::move(owned), "overload-test");
        adapter = std::make_unique<RadioResourceAdapter>(&radio, &store, "radio-1");
        backend->connected = true;
        emit radio.connectionStateChanged(true);
        call(service, client, "hello", {{"versions", QJsonArray{1}}});
    }
    bool advertised()
    {
        return service.capabilities(client).value("capabilities").toArray()
            .contains(QStringLiteral("frontEndOverload.read"));
    }
    QJsonObject value() const
    {
        const auto snapshot = store.get(address);
        return snapshot ? snapshot->value : QJsonObject{};
    }
};

void declaringBackendIsOffered()
{
    Fixture f(true);
    check(f.store.get(f.address).has_value() && f.advertised(),
          "a declaring, connected backend is offered frontEndOverload.read");
    check(f.value().value("level").toString()
              == RadioResourceAdapter::frontEndLevelName(FrontEndLevel::Unobserved),
          "before any window the level is unobserved, not clean");

    const quint64 sessionRevision = f.store.get({"radioSession", {}, "radio-1"})->revision;
    QSet<QString> names;
    for (FrontEndLevel level : kLevels) {
        FrontEndOverload s;
        s.level = level;
        s.autoArmed = level != FrontEndLevel::Unobserved;
        s.autoOffsetDb = static_cast<int>(level) * 3;
        s.reason = QStringLiteral("free text");
        emit f.radio.frontEndOverloadChanged(s);
        const QJsonObject v = f.value();
        const QJsonObject gain = v.value("autoGain").toObject();
        check(v.value("level").toString() == RadioResourceAdapter::frontEndLevelName(level)
                  && gain.value("armed").toBool() == s.autoArmed
                  && gain.value("offsetDb").toInt() == s.autoOffsetDb,
              "each level, the loop state and its offset reach the resource");
        names.insert(RadioResourceAdapter::frontEndLevelName(level));
    }
    check(names.size() == static_cast<qsizetype>(kLevels.size()) && !names.contains(QString()),
          "every level has its own non-empty wire name");
    const QStringList keys = f.value().keys();
    check(QSet<QString>(keys.cbegin(), keys.cend()) == QSet<QString>{"level", "autoGain"},
          "the backend's free-text reason is not projected");
    check(f.store.get({"radioSession", {}, "radio-1"})->revision == sessionRevision,
          "overload changes leave the radioSession revision alone");

    const QJsonObject got = call(f.service, f.client, "resource.get",
        {{"resource", QJsonObject{{"type", "frontEndOverload"}, {"radioSession", "radio-1"}}}});
    check(got.value("result").toObject().value("value").toObject() == f.value(),
          "resource.get returns the published value");
    const QJsonObject withId = call(f.service, f.client, "resource.get",
        {{"resource", QJsonObject{{"type", "frontEndOverload"}, {"radioSession", "radio-1"}, {"id", "0"}}}});
    const QJsonObject noSession = call(f.service, f.client, "resource.get",
        {{"resource", QJsonObject{{"type", "frontEndOverload"}}}});
    check(withId.contains("error") && noSession.contains("error"),
          "the selector takes radioSession and no id");
    const QJsonObject baseline = call(f.service, f.client, "resource.subscribe",
        {{"resources", QJsonArray{QJsonObject{{"type", "frontEndOverload"}, {"radioSession", "radio-1"}}}}});
    check(baseline.value("result").toObject().value("resources").toArray().size() == 1,
          "a subscription baseline carries it");

    f.backend->connected = false;
    emit f.radio.connectionStateChanged(false);
    check(!f.store.get(f.address) && !f.advertised(), "disconnect withdraws the resource");
    FrontEndOverload late;
    late.level = FrontEndLevel::Hot;
    emit f.radio.frontEndOverloadChanged(late);
    check(!f.store.get(f.address), "a late reading cannot resurrect it while disconnected");
}

void nonDeclaringBackendIsNotOffered()
{
    Fixture f(false);
    FrontEndOverload s;
    s.level = FrontEndLevel::Hot;
    emit f.radio.frontEndOverloadChanged(s);
    check(!f.store.get(f.address) && !f.advertised(),
          "a backend without the record is not offered, even if a reading arrives");

    f.backend->declares = true;
    emit f.radio.capabilitiesChanged(true, f.backend->capabilities());
    check(f.store.get(f.address).has_value() && f.advertised(), "declaring it later offers it");
    f.backend->declares = false;
    emit f.radio.capabilitiesChanged(true, f.backend->capabilities());
    check(!f.store.get(f.address) && !f.advertised(), "withdrawing the record withdraws the resource");
}

void productionDeclarations()
{
    // The HL2 decodes the converter's clip bit; no other family observes its ADC.
    for (const QString family : {"hl2", "flex", "sim", "icom", "anan", "rtl"}) {
        RadioModel radio;
        if (!radio.rebuildBackendForTest(family)) {
            std::printf("[SKIP] %s has no backend in this build\n", qPrintable(family));
            continue;
        }
        const bool declared = radio.backendCapabilities().frontEndOverloadObservation.has_value();
        const QByteArray label = (family + (family == "hl2" ? " declares the record"
                                                            : " does not declare the record")).toUtf8();
        check(declared == (family == "hl2"), label.constData());
        if (family == "hl2") {
            FrontEndOverload s;
            s.level = FrontEndLevel::Marginal;
            emit radio.backend()->frontEndOverloadChanged(s);
            check(radio.frontEndOverload() == s, "the HL2 backend's reading reaches RadioModel");
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("control-front-end-overload"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    declaringBackendIsOffered();
    nonDeclaringBackendIsNotOffered();
    productionDeclarations();
    return failures == 0 ? 0 : 1;
}
