#include "TestSettingsProfile.h"
#include "core/ClientDisplaySettings.h"
#include <QCoreApplication>
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
    return failed ? 1 : 0;
}
