// Radio-scoped settings persistence for the "Hermes Lite 2" misc-options
// page (Hl2MiscOptionsSettings) — a JSON feature document per radio via
// RadioSettingsScope (AGENTS.md "Radio-Scoped Feature Documents"), not a
// flat AppSettings key. Read-modify-write round trip, and a corrupt/absent
// document falling back to the documented default, per issue #9's Testing
// Decisions.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2MiscOptionsSettings.h"
#include "core/backends/hl2/MetisProtocol.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-misc-options-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const RadioSettingsScope radioA(QStringLiteral("hl2"), QStringLiteral("00:1C:C0:AA:AA:AA"));
    const RadioSettingsScope radioB(QStringLiteral("hl2"), QStringLiteral("00:1C:C0:BB:BB:BB"));

    // ---- Hl2MiscOptionsSettings: defaults on an untouched radio ----
    {
        check(!Hl2MiscOptionsSettings::adcDither(radioA), "adcDither defaults false");
        check(!Hl2MiscOptionsSettings::adcRandom(radioA), "adcRandom defaults false");
        check(!Hl2MiscOptionsSettings::resetOnDisconnect(radioA), "resetOnDisconnect defaults false");
        check(!Hl2MiscOptionsSettings::swapAudioChannels(radioA), "swapAudioChannels defaults false");
        check(Hl2MiscOptionsSettings::txLatency(radioA) == 20,
              "txLatency defaults to the reference client's own initial value (20)");
        check(Hl2MiscOptionsSettings::pttHang(radioA) == 12,
              "pttHang defaults to the reference client's own initial value (12)");
    }

    // ---- Hl2MiscOptionsSettings: read-modify-write round trip ----
    {
        Hl2MiscOptionsSettings::setAdcDither(radioA, true);
        check(Hl2MiscOptionsSettings::adcDither(radioA), "adcDither round-trips true");
        Hl2MiscOptionsSettings::setTxLatency(radioA, 100);
        Hl2MiscOptionsSettings::setPttHang(radioA, 25);
        check(Hl2MiscOptionsSettings::txLatency(radioA) == 100, "txLatency round-trips");
        check(Hl2MiscOptionsSettings::pttHang(radioA) == 25, "pttHang round-trips");
        // Setting one field must not clobber another already in the document
        // (Principle XIV: the document is a unit, but a writer must still
        // preserve fields it isn't touching).
        check(Hl2MiscOptionsSettings::adcDither(radioA),
              "setting txLatency/pttHang did not clobber the earlier adcDither write");

        // Out-of-range writes are refused rather than silently clamped or stored.
        Hl2MiscOptionsSettings::setTxLatency(radioA, hl2::kTxLatencyMax + 50);
        check(Hl2MiscOptionsSettings::txLatency(radioA) == 100,
              "an out-of-range TX latency write is refused, not clamped in");
        Hl2MiscOptionsSettings::setPttHang(radioA, -1);
        check(Hl2MiscOptionsSettings::pttHang(radioA) == 25,
              "a negative PTT hang write is refused, not clamped in");
    }

    // ---- Hl2MiscOptionsSettings: per-radio isolation ----
    {
        check(!Hl2MiscOptionsSettings::adcDither(radioB),
              "radio B's document is independent of radio A's (per-MAC scoping)");
        check(Hl2MiscOptionsSettings::txLatency(radioB) == 20,
              "radio B still reads the untouched default");
    }

    // ---- an invalid (empty-family) scope refuses writes and reads as empty ----
    {
        const RadioSettingsScope invalid;
        check(!invalid.isValid(), "a default-constructed scope is invalid");
        check(!Hl2MiscOptionsSettings::adcDither(invalid),
              "an invalid scope reads the documented default, not garbage");
        Hl2MiscOptionsSettings::setAdcDither(invalid, true);
        check(!Hl2MiscOptionsSettings::adcDither(invalid),
              "a write through an invalid scope does not silently succeed");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_misc_options_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
