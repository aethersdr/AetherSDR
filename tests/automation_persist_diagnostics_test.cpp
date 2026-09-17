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
    explicit StubBackend(QVariantList chains, bool twoTone = false)
        : m_chains(std::move(chains)), m_twoTone(twoTone) {}

    RadioCapabilities capabilities() const override
    {
        RadioCapabilities c;
        if (m_twoTone) {
            c.twoToneGenerator = RadioCapabilities::TwoToneGenerator{
                QStringLiteral("stub two-tone route")};
        }
        return c;
    }
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
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion& = {}) override {}
    void invokeExtension(const QString&, const QString&, quint64 id,
                         const QVariant&) override {
        emit extensionResult(id, QVariantMap{{"stateFreshness", QVariantMap{{"trackedStateReady", false}}}});
    }

    QVariantList dspChains() const override { return m_chains; }

private:
    QVariantList m_chains;
    bool m_twoTone = false;
};


int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("automation-persist-diagnostics"));
    qputenv("AETHER_AUTOMATION", "1");
    // DELIBERATELY NOT SET. The capability-before-TX-gate ordering below is
    // pinned by which refusal comes back, so nothing here needs TX armed -- and
    // a keying test whose safety rests on AutomationServer::start() never being
    // called is one refactor away from arming a real tune (Principle VI).
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
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
    // ONE DOCUMENT, ONE ANSWER ABOUT ONE SENSOR. `radiocert persist` embeds the
    // `radio` block, so a scalar there and a qualified null in `get meters`
    // means the same snapshot states both. Prove they agree in both directions:
    // a real zero reads zero here too, and an absent sensor reads null.
    const auto radioPaTemp = [&]() {
        return request("get radio").value("radio").toObject().value("paTemp");
    };
    check(radioPaTemp().isDouble() && radioPaTemp().toDouble() == 0,
        "radioSnapshot reports a real zero-degree sample as zero");
    radio.meterModel().removeMeter(1);
    // RE-READ the persist snapshot: the one captured above was built before
    // PATEMP was ever declared, so asserting against it could not fail and
    // pinned nothing about `radiocert persist` (#5516 review).
    check(radioPaTemp().isNull() && meters().value("paTemp").isNull()
        && request("radiocert persist").value("radio").toObject()
               .value("paTemp").isNull(),
        "an undeclared sensor is null in radioSnapshot exactly as in get meters");

    // THE SUPPORT-BUNDLE SURFACE ANSWERS THE SAME WAY. troubleshootingSnapshot()
    // is what an operator pastes into a support thread, and it read the scalar
    // directly -- so a radio with no temperature meter at all (every Icom)
    // reported "PA 0.00 C" as a measurement while `get meters` next door said
    // `unsupported` (#5516 review).
    const auto troubleshooting = [&]() {
        return radio.troubleshootingSnapshot().value("radio").toObject()
            .value("telemetry").toObject();
    };
    check(troubleshooting().value("pa_temp_c").isNull()
        && troubleshooting().value("supply_volts").isNull(),
        "an undeclared sensor is null in the troubleshooting snapshot too");
    MeterDef again;
    again.index = 2; again.source = "RAD"; again.name = "PATEMP"; again.unit = "degC";
    radio.meterModel().defineMeter(again);
    check(troubleshooting().value("pa_temp_c").isNull(),
        "a declared but never-fed sensor is still null there");
    radio.meterModel().updateValueByName("RAD", "PATEMP", 41.5f);
    check(troubleshooting().value("pa_temp_c").toDouble() == 41.5,
        "a real reading is reported as itself");
    // EVER-FED IS NOT CURRENT. hasPaTemp() stays true once a sample lands and
    // is only cleared when the meter definition goes, so gating on it alone
    // would keep reporting a sensor that stopped an hour ago -- while
    // `get meters` next door called it stale. Both surfaces now run the same
    // predicate over the same window, so pin the predicate and the age source
    // rather than adding a production hook to backdate a sample (#5516 review).
    check(MeterModel::vitalIsFresh(true, 0)
        && !MeterModel::vitalIsFresh(true, MeterModel::kVitalsFreshMs)
        && !MeterModel::vitalIsFresh(true, -1)
        && !MeterModel::vitalIsFresh(false, 0),
        "the vitals window rejects stale, never-fed and undeclared alike");
    check(radio.meterModel().paTempAgeMs() >= 0,
        "a fed sensor reports a real age");
    radio.meterModel().removeMeter(2);
    check(radio.meterModel().paTempAgeMs() == -1,
        "and an undeclared one reports no age at all");

    // CAPABILITY-SHAPED, NOT FAMILY-SHAPED. The refusal must follow "this
    // backend has no two-tone generator", which is what makes it cover HL2 and
    // every other single-carrier tune producer rather than only Icom.
    const auto twoTone = request("txtest twotone");
    check(!twoTone.value("ok").toBool()
        && twoTone.value("error").toString().contains("not implemented")
        && !radio.transmitModel().isTuning() && !radio.transmitModel().isMox(),
        "a backend without a two-tone generator cannot label one tone as two");
    // The HL2 case, which is what makes this a capability and not a family
    // check: a non-Icom family whose tune producer is still a single carrier
    // must be refused. A `family() == "icom"` guard waves this one through.
    radio.setBackendForTest(std::make_unique<StubBackend>(QVariantList{}, /*twoTone=*/false), "hl2");
    check(request("txtest twotone").value("error").toString().contains("not implemented")
        && !radio.transmitModel().isTuning(),
        "a non-Icom family without a two-tone route is refused just the same");
    radio.setBackendForTest(std::make_unique<StubBackend>(QVariantList{}, /*twoTone=*/true), "icom");
    // ASSERT THE ORDERING, not merely the absence of one refusal string: the
    // previous form was satisfied by ANY other error, so it could pass without
    // the capability branch ever being taken. A declared generator must clear
    // the capability gate and stop at the NEXT one instead -- which pins both
    // halves of the claim, that the check is capability-shaped and that it sits
    // in front of the TX gate (#5516 review).
    //
    // The TX gate is closed because this test never arms it -- see the
    // qunsetenv above -- not because start() happens not to run.
    const auto allowed = request("txtest twotone");
    check(!allowed.value("ok").toBool()
        && !allowed.value("error").toString().contains("not implemented")
        && allowed.value("error").toString().contains("AETHER_AUTOMATION_ALLOW_TX"),
        "a declared generator clears the capability gate and stops at the TX gate");
    check(!radio.transmitModel().isTuning() && !radio.transmitModel().isMox(),
        "and nothing along that path keyed the transmitter");
    return failures ? 1 : 0;
}
