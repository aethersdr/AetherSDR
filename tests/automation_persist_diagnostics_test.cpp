// Socket-free bridge contract: diagnostics must not turn missing data into proof.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <cmath>
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

    // ── #5499 items 1 and 2: two more scalars that answered when they had
    // nothing to answer with. Same file as paTemp above because it is the same
    // contract — a published field either carries a value or says it has none.
    //
    // `get radio`.txPower published RadioModel::m_txPower: declared, given a
    // getter and a Q_PROPERTY(float txPower READ txPower NOTIFY metersChanged),
    // and ASSIGNED NOWHERE IN THE TREE. It read 0 at every drive. A bench run
    // gated a key on setting a drive and reading it back here, and the gate
    // compared 0 against 0 — the right shape of check, incapable of failing.
    //
    // `get meters`.sLevel published MeterModel::m_sLevel, written only by
    // MeterModel::clear() to -130.0f, so it answered -130 dBm for 1304
    // consecutive samples while the SLC:LEVEL row in the SAME reply moved
    // around a median of -83.9.
    //
    // Each of the three states is asserted, and the FRESH state is the positive
    // control for the two nulls: without it, "reports null" would be satisfied
    // by a field that had simply been deleted, or by a test that cannot reach
    // it — which is the same fabricated-confidence failure wearing the opposite
    // sign.
    const auto radioTxPower = [&]() {
        return request("get radio").value("radio").toObject().value("txPower");
    };
    check(radioTxPower().isNull(),
        "a radio with no forward-power meter reports no transmit power");
    MeterDef fwd;
    fwd.index = 3; fwd.source = "TX-"; fwd.sourceIndex = 8;
    fwd.name = "FWDPWR"; fwd.unit = "Watts";
    radio.meterModel().defineMeter(fwd);
    check(radioTxPower().isNull(),
        "a declared but never-fed forward-power meter is still not a reading");
    // 0.153 W is the low end of what the bench measured into the dummy load on
    // the run where this field read 0 throughout.
    radio.meterModel().updateValueByName("TX-", "FWDPWR", 0.153f);
    const QJsonValue txPowerFresh = radioTxPower();
    check(txPowerFresh.isDouble()
        && std::fabs(txPowerFresh.toDouble() - 0.153) < 1e-4,
        "and a real forward-power sample reaches get radio as itself");
    // ONE DOCUMENT, ONE ANSWER, exactly as for paTemp above: the scalar in
    // `get radio` and the meter in `get meters` are the same measurement and
    // must not be able to differ.
    check(std::fabs(meters().value("fwdPower").toDouble() - txPowerFresh.toDouble()) < 1e-9,
        "get radio.txPower is the same number as get meters.fwdPower");
    radio.meterModel().removeMeter(3);
    check(radioTxPower().isNull(),
        "and removing the meter takes the reading with it");
    // ...AND THE SAMPLE WITH IT, WHICH THE ASSERTION ABOVE CANNOT SEE.
    // removeMeter() cleared m_fwdPwrIdx and nothing else, so the check above
    // passes on the index alone while m_fwdPower and m_lastFwdPowerUpdateMs
    // still hold the departed meter's reading. fwdPowerIfLive() qualifies on
    // that STAMP: declare any FWDPWR meter again inside kTxMeterStaleMs and
    // the stale stamp is still inside the window, so `get radio`.txPower
    // answers 0.153 W for a meter that has carried no packet -- the
    // declared-but-never-fed case this file asserts is null, arriving by a
    // door the never-fed assertion above does not watch. Found by ten9876
    // reviewing #5849; REFPWR's branch in removeMeter() already zeroed both of
    // its members, which is what made the asymmetry visible.
    //
    // The re-declaration uses a DIFFERENT index on purpose: the bug is in the
    // sample, not in index reuse, and a fresh index proves it without relying
    // on defineMeter()'s own removeMeter() call.
    MeterDef fwdAgain;
    fwdAgain.index = 5; fwdAgain.source = "TX-"; fwdAgain.sourceIndex = 9;
    fwdAgain.name = "FWDPWR"; fwdAgain.unit = "Watts";
    radio.meterModel().defineMeter(fwdAgain);
    check(radioTxPower().isNull(),
        "a FWDPWR meter declared just after a removal is unfed, not the old watts");
    // The positive control for that null, and a second reading of the same
    // defect: the EMA's first-sample branch is `m_fwdPower < 0.01f`. With the
    // old watts left standing, the replacement's FIRST packet arrives already
    // smoothed against a meter it never shared a radio with -- 0.5*0.184 +
    // 0.5*0.153 = 0.1685 -- so this assertion fails on the value as well as
    // the null, and for a reason a client could never diagnose.
    radio.meterModel().updateValueByName("TX-", "FWDPWR", 0.184f);
    const QJsonValue txPowerAgain = radioTxPower();
    check(txPowerAgain.isDouble()
        && std::fabs(txPowerAgain.toDouble() - 0.184) < 1e-4,
        "and its first sample is its own, not smoothed against the removed meter's");
    radio.meterModel().removeMeter(5);
    check(radioTxPower().isNull(),
        "and the replacement's removal leaves nothing behind either");

    const auto sLevel = [&]() { return meters().value("sLevel"); };
    check(sLevel().isNull(),
        "a radio with no S-meter reports no S-level");
    MeterDef slc0;
    slc0.index = 4; slc0.source = "SLC"; slc0.sourceIndex = 0;
    slc0.name = "LEVEL"; slc0.unit = "dBm"; slc0.low = -150.0; slc0.high = 20.0;
    radio.meterModel().defineMeter(slc0);
    check(sLevel().isNull(),
        "a declared but never-fed S-meter is still not a reading");
    radio.meterModel().updateValueByName("SLC", "LEVEL", -83.9f, 0);
    check(sLevel().isDouble() && std::fabs(sLevel().toDouble() + 83.9) < 1e-4,
        "and a real S-meter sample reaches get meters as itself");
    // THE ARRAY AND THE SCALAR, checked against each other rather than against
    // a number typed twice. #4533's rule is that a client reading the scalar
    // must not get a different answer from one reading the array; the old
    // scalar broke it by 46 dB.
    const auto rowValue = [&](const QString& source, const QString& name) {
        const QJsonArray all = meters().value("all").toArray();
        for (const QJsonValue& v : all) {
            const QJsonObject row = v.toObject();
            if (row.value("source").toString() == source
                && row.value("name").toString() == name) {
                return row.value("value");
            }
        }
        return QJsonValue();
    };
    check(std::fabs(rowValue("SLC", "LEVEL").toDouble() - sLevel().toDouble()) < 1e-9,
        "the sLevel scalar equals the SLC:LEVEL row it is published beside");

    // A SECOND RECEIVER MAKES THE SCALAR AMBIGUOUS, and the honest answer to an
    // ambiguous question is not one of the two candidates. Answering "whichever
    // slice updated last" is the bug #155 fixed; the per-slice rows in `all`
    // are still there for a client that means a particular receiver.
    MeterDef slc1;
    slc1.index = 5; slc1.source = "SLC"; slc1.sourceIndex = 1;
    slc1.name = "LEVEL"; slc1.unit = "dBm"; slc1.low = -150.0; slc1.high = 20.0;
    radio.meterModel().defineMeter(slc1);
    radio.meterModel().updateValueByName("SLC", "LEVEL", -51.4f, 1);
    check(sLevel().isNull(),
        "with two receivers the scalar declines rather than picking one");
    // CONTROL for that null: both receivers are live and DIFFERENT, so the
    // decline is about ambiguity and not about the meters having gone away.
    const auto s0 = radio.meterModel().sLevelForSlice(0);
    const auto s1 = radio.meterModel().sLevelForSlice(1);
    check(s0 && s1 && std::fabs(*s0 - *s1) > 30.0f,
        "while each receiver still reports its own, clearly different, level");

    return failures ? 1 : 0;
}
