// ANAN-G2 — owned settings object ("Anan" root key, Principle V).
//
// No credential concern here (unlike icom_settings_test.cpp) -- this backend
// has nothing secret to leak. The load-bearing assertions are the round-trip
// and "never a bogus default" guards: an out-of-range/hand-edited rate must
// not silently commit to something AnanBackend would then have to snap anyway.
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/backends/anan/AnanSettings.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::anan;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    // BEFORE QCoreApplication and before the first AppSettings touch — the
    // profile redirects the settings home, and Qt caches those paths.
    TestSettingsProfile profile(QStringLiteral("anan-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- defaults on an empty document -------------------------------------
    AnanSettings::reset();
    check(AnanSettings::ddc0RateKsps() == 48, "default DDC0 rate is 48 ksps");

    // ---- round trip ----------------------------------------------------------
    AnanSettings::setDdc0RateKsps(384);
    check(AnanSettings::ddc0RateKsps() == 384, "rate round-trips");

    // A hand-edited or truncated file must not command a nonsense rate —
    // AnanBackend::nearestDdc0RateKsps() is the actual snap-to-valid-rate
    // authority, so this class only needs to keep a non-positive value from
    // reaching it as a literal.
    AppSettings::instance().setValue(QStringLiteral("Anan"),
                                     QStringLiteral(R"({"ddc0RateKsps":0})"));
    check(AnanSettings::ddc0RateKsps() == 48, "a zero rate falls back to the default");

    // ---- ADC options -----------------------------------------------------
    AnanSettings::reset();
    check(AnanSettings::ditherEnabled() && AnanSettings::randomEnabled(),
          "dither/random default on");
    check(AnanSettings::ddc0AdcIndex() == 0, "default ADC selection is ADC0");
    check(AnanSettings::bypassAdc0Filters() && AnanSettings::bypassAdc1Filters(),
          "both bypass options default on");

    AnanSettings::setDitherEnabled(false);
    AnanSettings::setRandomEnabled(false);
    AnanSettings::setDdc0AdcIndex(1);
    AnanSettings::setBypassAdc0Filters(false);
    AnanSettings::setBypassAdc1Filters(false);
    check(!AnanSettings::ditherEnabled() && !AnanSettings::randomEnabled(),
          "dither/random round-trip off");
    check(AnanSettings::ddc0AdcIndex() == 1, "ADC1/RX2 selection round-trips");
    check(!AnanSettings::bypassAdc0Filters() && !AnanSettings::bypassAdc1Filters(),
          "both bypass options round-trip off");

    // A hand-edited value outside {0,1} must not reach P2Client as a
    // literal ADC index it was never validated against.
    AppSettings::instance().setValue(QStringLiteral("Anan"),
                                     QStringLiteral(R"({"ddc0AdcIndex":7})"));
    check(AnanSettings::ddc0AdcIndex() == 0, "an out-of-range ADC index falls back to ADC0");

    // ---- step attenuation, per ADC ---------------------------------------
    AnanSettings::reset();
    check(AnanSettings::adcAttenuationDb(0) == 0 && AnanSettings::adcAttenuationDb(1) == 0,
          "both attenuators default to 0 dB");
    AnanSettings::setAdcAttenuationDb(1, 20);
    check(AnanSettings::adcAttenuationDb(1) == 20, "ADC1 attenuation round-trips");
    check(AnanSettings::adcAttenuationDb(0) == 0, "setting ADC1 leaves ADC0 alone");
    AnanSettings::setAdcAttenuationDb(0, 45);
    check(AnanSettings::adcAttenuationDb(0) == 31, "a write above 31 dB is stored as 31");

    // A hand-edited document must not reach the High Priority packet out of
    // the spec's 0-31 dB range.
    AppSettings::instance().setValue(
        QStringLiteral("Anan"),
        QStringLiteral(R"({"adc0AttenuationDb":42,"adc1AttenuationDb":-5})"));
    check(AnanSettings::adcAttenuationDb(0) == 31, "a hand-edited 42 dB reads as 31");
    check(AnanSettings::adcAttenuationDb(1) == 0, "a hand-edited -5 dB reads as 0");

    // An ADC this radio does not have reads 0 and writes nothing.
    AnanSettings::reset();
    AnanSettings::setAdcAttenuationDb(1, 7);
    AnanSettings::setAdcAttenuationDb(2, 10);
    check(AnanSettings::adcAttenuationDb(2) == 0, "an unknown ADC index reads 0 dB");
    check(AnanSettings::adcAttenuationDb(0) == 0 && AnanSettings::adcAttenuationDb(1) == 7,
          "a write to an unknown ADC index changes nothing");

    if (g_failures == 0)
        std::printf("anan_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
