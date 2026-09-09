// Socket-free bridge contract: diagnostics must not turn missing data into proof.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <cstdio>
#include <memory>
#include <utility>
namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line) {
        return server.handleLine(line, nullptr);
    }
};
}
using namespace AetherSDR;
class StubBackend : public IRadioBackend
{
public:
    explicit StubBackend(QVariantList chains) : m_chains(std::move(chains)) {}

    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    // Deliberately inert. This test never calls it, and there is no wire behind
    // it if it did.
    void setKeying(bool) override {}
    void invokeExtension(const QString&, const QString&, quint64 id,
                         const QVariant&) override {
        emit extensionResult(id, QVariantMap{{"stateFreshness", QVariantMap{{"trackedStateReady", false}}}});
    }

    QVariantList dspChains() const override { return m_chains; }

private:
    QVariantList m_chains;
};


int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("automation-persist-diagnostics"));
    qputenv("AETHER_AUTOMATION", "1");
    qputenv("AETHER_AUTOMATION_ALLOW_TX", "1");
    QCoreApplication app(argc, argv);
    if (!profile.isValid()) { return 1; }
    RadioModel radio;
    AutomationServer server;
    server.setRadioModel(&radio);
    const auto request = [&](const QByteArray& line) {
        return AutomationServerTestAccess::request(server, line);
    };
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
        if (!ok) { ++failures; }
    };
    radio.setBackendForTest(std::make_unique<StubBackend>(QVariantList{}), "icom");
    const auto persist = request("radiocert persist");
    check(persist.value("ok").toBool() && persist.value("backendDiagnostics").toObject()
        .value("result").toObject().value("stateFreshness").toObject().contains("trackedStateReady"),
        "persist carries backend confirmation evidence without replacing model state");
    const auto meters = [&]() { return request("get meters").value("meters").toObject(); };
    check(meters().value("paTemp").isNull()
        && meters().value("temperature").toObject().value("status") == "unsupported",
        "undefined temperature is null and unsupported, not zero degrees");
    MeterDef def;
    def.index = 1; def.source = "RAD"; def.name = "PATEMP"; def.unit = "degC";
    radio.meterModel().defineMeter(def);
    check(meters().value("paTemp").isNull()
        && meters().value("temperature").toObject().value("status") == "never-fed",
        "defined but never-fed temperature stays unknown");
    radio.meterModel().updateValueByName("RAD", "PATEMP", 0.0f);
    check(meters().value("paTemp").isDouble() && meters().value("paTemp").toDouble() == 0
        && meters().value("temperature").toObject().value("status") == "fresh",
        "a real zero-degree sample is distinguished from an absent reading");
    const auto twoTone = request("txtest twotone");
    check(!twoTone.value("ok").toBool()
        && twoTone.value("error").toString().contains("not implemented")
        && !radio.transmitModel().isTuning() && !radio.transmitModel().isMox(),
        "Icom cannot label its single-tone generator as two-tone or key on refusal");
    return failures ? 1 : 0;
}
