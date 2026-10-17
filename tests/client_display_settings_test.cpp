#include "TestSettingsProfile.h"
#include "core/ClientDisplaySettings.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QString>
#include <cstdio>

using namespace AetherSDR;
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("client-display-settings"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    int failed = 0;
    const auto check = [&failed](bool ok, const char* name) {
        if (!ok) { ++failed; std::fprintf(stderr, "FAIL: %s\n", name); }
    };
    const RadioSettingsScope a(QStringLiteral("icom"), QStringLiteral("A"));
    const RadioSettingsScope b(QStringLiteral("icom"), QStringLiteral("B"));
    const RadioSettingsScope flex(QStringLiteral("flex"), QStringLiteral("A"));
    const RadioSettingsScope unknown(QStringLiteral("icom"), {});
    check(!ClientDisplaySettings::waterfallRate(a, 0, true), "missing remains unconfigured");
    ClientDisplaySettings::saveWaterfallRate(a, 0, true, 63);
    ClientDisplaySettings::saveWaterfallRate(a, 1, true, 42);
    check(ClientDisplaySettings::waterfallRate(a, 0, true) == 63, "pan zero retained");
    check(ClientDisplaySettings::waterfallRate(a, 1, true) == 42, "pan one isolated");
    check(!ClientDisplaySettings::waterfallRate(b, 0, true), "radio identities isolated");
    ClientDisplaySettings::saveWaterfallRate(flex, 0, false, 63);
    check(flex.featureExact(QStringLiteral("ClientDisplay")).isEmpty(), "radio-owned cadence never saved");
    check(!ClientDisplaySettings::waterfallRate(a, 0, false), "radio-owned cadence never restored");
    ClientDisplaySettings::saveWaterfallRate(unknown, 0, true, 63);
    check(unknown.featureExact(QStringLiteral("ClientDisplay")).isEmpty(), "unknown identity never writes family default");
    ClientDisplaySettings::saveWaterfallRate(a, 0, true, 0);
    ClientDisplaySettings::saveWaterfallRate(a, 0, true, 101);
    check(ClientDisplaySettings::waterfallRate(a, 0, true) == 63, "invalid writes preserve original");
    const QJsonObject future{{QStringLiteral("waterfallRates"), QJsonObject{{QStringLiteral("0"), 77}}}};
    check(b.setFeature(QStringLiteral("ClientDisplay"), 2, future), "future fixture stored");
    ClientDisplaySettings::saveWaterfallRate(b, 0, true, 12);
    check(!ClientDisplaySettings::waterfallRate(b, 0, true), "future schema not interpreted");
    check(b.featureExact(QStringLiteral("ClientDisplay")) == future, "future schema not overwritten");
    check(a.setFeature(QStringLiteral("ClientDisplay"), 1,
        {{QStringLiteral("waterfallRates"), QJsonObject{{QStringLiteral("0"), 63.5}}}}), "fraction fixture stored");
    check(!ClientDisplaySettings::waterfallRate(a, 0, true), "fractional values rejected");
    const RadioSettingsScope rtl("rtl", "averaging-A"), other("rtl", "averaging-B");
    using Average = ClientDisplaySettings::FftAverage;
    check(!ClientDisplaySettings::fftAverage(rtl,0,true), "absent averaging stays unconfigured");
    check(ClientDisplaySettings::saveFftAverage(rtl,0,true,{73,true}), "local averaging saves both controls");
    ClientDisplaySettings::saveWaterfallRate(rtl,0,true,63);
    check(ClientDisplaySettings::fftAverage(rtl,0,true)==Average{73,true}
        && ClientDisplaySettings::waterfallRate(rtl,0,true)==63, "averaging and waterfall owners preserve each other's fields");
    check(!ClientDisplaySettings::fftAverage(other,0,true)
        && !ClientDisplaySettings::fftAverage(rtl,1,true), "averaging is isolated by exact radio and pan");
    check(!ClientDisplaySettings::saveFftAverage(rtl,0,true,{101,false})
        && ClientDisplaySettings::fftAverage(rtl,0,true)==Average{73,true}, "invalid average cannot overwrite saved choice");
    check(!ClientDisplaySettings::saveFftAverage(other,0,false,{42,true})
        && !ClientDisplaySettings::fftAverage(rtl,0,false)
        && other.featureExact("ClientDisplay").isEmpty(), "undeclared client persistence changes no other backend behavior");
    check(!ClientDisplaySettings::saveFftAverage(unknown,0,true,{42,true}), "unknown device cannot write averaging fallback");
    check(!ClientDisplaySettings::saveFftAverage(RadioSettingsScope::anonymousRadio("rtl"),0,true,{42,true}),
          "anonymous display choices stay session-only instead of becoming another dongle's fallback");
    check(other.setFeature("ClientDisplay",99,{{"future",true}}), "future averaging fixture stored");
    check(!ClientDisplaySettings::saveFftAverage(other,0,true,{42,true})
        && !ClientDisplaySettings::fftAverage(other,0,true)
        && other.featureExact("ClientDisplay").value("future").toBool(), "future averaging document is preserved");
    for (const QJsonValue bad : {QJsonValue(17.3),QJsonValue("17"),QJsonValue(true)}) {
        QJsonObject malformed{{"fftAverages",QJsonObject{{"0",QJsonObject{{"average",bad},{"weighted",true}}}}}};
        check(other.setFeature("ClientDisplay",1,malformed), "malformed averaging fixture stored");
        check(!ClientDisplaySettings::fftAverage(other,0,true)
            && !ClientDisplaySettings::saveFftAverage(other,0,true,{42,false})
            && other.featureExact("ClientDisplay")==malformed, "malformed average is neither coerced nor overwritten");
    }
    // The waterfall rate on an anonymous radio (a dongle with no serial): its
    // scope addresses the family-wide row, so nothing is stored or restored.
    {
        const QString feature = QStringLiteral("ClientDisplay");
        const RadioSettingsScope anonymous = RadioSettingsScope::anonymousRadio("rtl");
        const RadioSettingsScope rtlFamily("rtl", {});
        const auto familyRow = [&feature](AppSettings::FeatureReadStatus* status) {
            return AppSettings::instance().radioFeatureExact(
                QStringLiteral("rtl"), QString(), feature, nullptr, status);
        };
        AppSettings::FeatureReadStatus status = AppSettings::FeatureReadStatus::Present;
        check(anonymous.hasRadioIdentity() && anonymous.radioId().isEmpty()
                  && !ClientDisplaySettings::keepsFor(anonymous)
                  && familyRow(&status).isEmpty()
                  && status == AppSettings::FeatureReadStatus::Missing,
              "control: an anonymous scope is a connected radio whose row is the family's, and none exists yet");
        ClientDisplaySettings::saveWaterfallRate(anonymous, 0, true, 63);
        check(!ClientDisplaySettings::waterfallRate(anonymous, 0, true),
              "an anonymous radio's waterfall rate is not restored");
        status = AppSettings::FeatureReadStatus::Present;
        check(familyRow(&status).isEmpty()
                  && status == AppSettings::FeatureReadStatus::Missing,
              "an anonymous radio's waterfall rate writes no family-wide row");

        // A radio with an identity keeps its rate, and only under that identity.
        const RadioSettingsScope hl2("hl2", "00:1C:C0:A2:13:DD");
        const RadioSettingsScope dongle("rtl", "00000001");
        ClientDisplaySettings::saveWaterfallRate(hl2, 0, true, 37);
        ClientDisplaySettings::saveWaterfallRate(dongle, 0, true, 58);
        check(ClientDisplaySettings::keepsFor(hl2)
                  && ClientDisplaySettings::waterfallRate(hl2, 0, true) == 37,
              "an HL2's waterfall rate is stored and restored");
        check(ClientDisplaySettings::keepsFor(dongle)
                  && ClientDisplaySettings::waterfallRate(dongle, 0, true) == 58,
              "a dongle with a serial has its waterfall rate stored and restored");
        check(hl2.featureExact(feature).value(QStringLiteral("waterfallRates")).toObject()
                      == QJsonObject{{QStringLiteral("0"), 37}}
                  && dongle.featureExact(feature).value(QStringLiteral("waterfallRates")).toObject()
                      == QJsonObject{{QStringLiteral("0"), 58}},
              "each identified radio's rate is in its own exact row");
        status = AppSettings::FeatureReadStatus::Present;
        check(familyRow(&status).isEmpty()
                  && status == AppSettings::FeatureReadStatus::Missing
                  && RadioSettingsScope("hl2", {}).featureExact(feature).isEmpty(),
              "and identified radios write no family-wide row either");

        // A family-wide row that is already there: left alone, and not applied.
        const QJsonObject older{
            {QStringLiteral("waterfallRates"), QJsonObject{{QStringLiteral("0"), 41}}}};
        check(rtlFamily.setFeature(feature, 1, older), "family-wide fixture stored");
        check(!ClientDisplaySettings::waterfallRate(anonymous, 0, true),
              "an existing family-wide waterfall rate is not applied to an anonymous radio");
        check(ClientDisplaySettings::waterfallRate(dongle, 0, true) == 58
                  && !ClientDisplaySettings::waterfallRate(RadioSettingsScope("rtl", "00000002"), 0, true),
              "nor to a dongle with a serial, with or without a rate of its own");
        ClientDisplaySettings::saveWaterfallRate(anonymous, 0, true, 63);
        status = AppSettings::FeatureReadStatus::Missing;
        check(familyRow(&status) == older
                  && status == AppSettings::FeatureReadStatus::Present,
              "an existing family-wide row is neither rewritten nor removed");
    }
    return failed ? 1 : 0;
}
