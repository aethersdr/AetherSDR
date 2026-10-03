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

    // ── FFT averaging and the dBm range ──────────────────────────────────
    // Added after a G2 bench run: both were being stored under global
    // per-widget keys and restored too late to survive the first status echo.
    // Here they are per radio and per pan, like the cadence above.
    const RadioSettingsScope c(QStringLiteral("anan"), QStringLiteral("C"));
    const RadioSettingsScope d(QStringLiteral("anan"), QStringLiteral("D"));

    check(!ClientDisplaySettings::fftAverage(c, 0), "averaging starts unconfigured");
    ClientDisplaySettings::saveFftAverage(c, 0, 6);
    ClientDisplaySettings::saveFftAverage(c, 1, 9);
    check(ClientDisplaySettings::fftAverage(c, 0) == 6, "averaging retained for pan zero");
    check(ClientDisplaySettings::fftAverage(c, 1) == 9, "averaging isolated per pan");
    check(!ClientDisplaySettings::fftAverage(d, 0), "averaging isolated per radio");
    // Zero is a REAL selection here, unlike the cadence, where it is out of
    // range: "no averaging" is what the slider's left end means.
    ClientDisplaySettings::saveFftAverage(c, 0, 0);
    check(ClientDisplaySettings::fftAverage(c, 0) == 0, "zero frames is a stored value, not an unset one");
    ClientDisplaySettings::saveFftAverage(c, 0, 6);
    ClientDisplaySettings::saveFftAverage(c, 0, -1);
    ClientDisplaySettings::saveFftAverage(c, 0, ClientDisplaySettings::kMaxFftAverage + 1);
    check(ClientDisplaySettings::fftAverage(c, 0) == 6, "out-of-range averaging writes preserve the original");
    ClientDisplaySettings::saveFftAverage(unknown, 0, 6);
    check(!ClientDisplaySettings::fftAverage(unknown, 0), "averaging never written without a radio identity");

    check(!ClientDisplaySettings::fftWeightedAverage(c, 0), "weighting starts unconfigured");
    ClientDisplaySettings::saveFftWeightedAverage(c, 0, true);
    check(ClientDisplaySettings::fftWeightedAverage(c, 0) == true, "weighting retained");
    // FALSE must round-trip as a stored false, not read back as "never set" --
    // otherwise turning weighting off could never survive a reconnect.
    ClientDisplaySettings::saveFftWeightedAverage(c, 0, false);
    const auto weighted = ClientDisplaySettings::fftWeightedAverage(c, 0);
    check(weighted.has_value() && *weighted == false,
          "weighting off is a stored value, distinct from unset");

    check(!ClientDisplaySettings::dbmRange(c, 0), "range starts unconfigured");
    ClientDisplaySettings::saveDbmRange(c, 0, -120.0f, -20.0f);
    const auto range = ClientDisplaySettings::dbmRange(c, 0);
    check(range && range->minDbm == -120.0f && range->maxDbm == -20.0f, "range round-trips");
    check(!ClientDisplaySettings::dbmRange(d, 0), "range isolated per radio");
    check(!ClientDisplaySettings::dbmRange(c, 1), "range isolated per pan");
    // Inverted and equal are both refused on the way IN: an inverted range
    // renders as an empty scale with no way back from the UI.
    ClientDisplaySettings::saveDbmRange(c, 0, -20.0f, -120.0f);
    ClientDisplaySettings::saveDbmRange(c, 0, -50.0f, -50.0f);
    ClientDisplaySettings::saveDbmRange(c, 0, -400.0f, -20.0f);
    ClientDisplaySettings::saveDbmRange(c, 0, -120.0f, 900.0f);
    const auto stillThere = ClientDisplaySettings::dbmRange(c, 0);
    check(stillThere && stillThere->minDbm == -120.0f && stillThere->maxDbm == -20.0f,
          "every invalid range write preserves the original");
    // And refused on the way OUT, because a document can be hand-edited.
    check(c.setFeature(QStringLiteral("ClientDisplay"), 1,
        {{QStringLiteral("dbmRanges"),
          QJsonObject{{QStringLiteral("0"),
                       QJsonObject{{QStringLiteral("min"), -20},
                                   {QStringLiteral("max"), -120}}}}}}),
          "inverted fixture stored");
    check(!ClientDisplaySettings::dbmRange(c, 0), "an inverted stored range is refused on read");
    check(c.setFeature(QStringLiteral("ClientDisplay"), 1,
        {{QStringLiteral("dbmRanges"),
          QJsonObject{{QStringLiteral("0"), QJsonObject{{QStringLiteral("min"), -120}}}}}}),
          "half-range fixture stored");
    check(!ClientDisplaySettings::dbmRange(c, 0), "half a range is not a range");

    return failed ? 1 : 0;
}
