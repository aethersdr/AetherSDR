// Radio-scoped settings persistence for the "Filter Board" manual override
// (Hl2FilterBoardSettings) — a JSON feature document per radio via
// RadioSettingsScope (AGENTS.md "Radio-Scoped Feature Documents"), not a
// flat AppSettings key. Read-modify-write round trip, and a corrupt/absent
// document falling back to the documented default, per issue #9's Testing
// Decisions.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2FilterBoard.h"
#include "core/backends/hl2/Hl2FilterBoardSettings.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::hl2;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("hl2-filter-board-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    const RadioSettingsScope radioA(QStringLiteral("hl2"), QStringLiteral("00:1C:C0:AA:AA:AA"));
    const RadioSettingsScope radioB(QStringLiteral("hl2"), QStringLiteral("00:1C:C0:BB:BB:BB"));

    // ---- Hl2FilterBoardSettings: defaults + round trip ----
    {
        check(!Hl2FilterBoardSettings::manualEnabled(radioA),
              "manual filter control defaults OFF (automatic stays the default)");
        check(Hl2FilterBoardSettings::table(radioA).isEmpty(),
              "an untouched radio's manual table is empty, not a guessed default");

        Hl2FilterBoardSettings::setManualEnabled(radioA, true);
        check(Hl2FilterBoardSettings::manualEnabled(radioA), "manualEnabled round-trips true");

        ManualFilterTable t;
        t.insert(QStringLiteral("40m"), ManualFilterBand{kOcLpf60_40, kOcHpfAmBc | kOcLpf60_40});
        t.insert(QStringLiteral("20m"), ManualFilterBand{kOcLpf30_20, kOcLpf30_20});
        Hl2FilterBoardSettings::setTable(radioA, t);

        const ManualFilterTable roundTripped = Hl2FilterBoardSettings::table(radioA);
        check(roundTripped.size() == 2, "the table round-trips with exactly the bands written");
        check(manualFilterByte(roundTripped, QStringLiteral("40m"), FilterDirection::Receive)
                  == kOcLpf60_40,
              "40m's RX mask round-trips");
        check(manualFilterByte(roundTripped, QStringLiteral("40m"), FilterDirection::Transmit)
                  == (kOcHpfAmBc | kOcLpf60_40),
              "40m's TX mask round-trips independently of RX");
        check(manualFilterByte(roundTripped, QStringLiteral("17m"), FilterDirection::Receive)
                  == kOcNone,
              "a band never written still reads back as kOcNone, not garbage");

        // manualEnabled must survive the table write untouched (Principle XIV:
        // one document, but a writer must still preserve fields it isn't
        // touching).
        check(Hl2FilterBoardSettings::manualEnabled(radioA),
              "writing the table did not clobber the earlier manualEnabled write");
    }

    // ---- Hl2FilterBoardSettings: per-radio isolation ----
    {
        check(!Hl2FilterBoardSettings::manualEnabled(radioB),
              "radio B's manual-mode flag is independent of radio A's");
        check(Hl2FilterBoardSettings::table(radioB).isEmpty(),
              "radio B's table is independent of radio A's");
    }

    // ---- an invalid (empty-family) scope refuses writes and reads as empty ----
    {
        const RadioSettingsScope invalid;
        check(!invalid.isValid(), "a default-constructed scope is invalid");
        check(!Hl2FilterBoardSettings::manualEnabled(invalid),
              "an invalid scope reads the documented default, not garbage");
        Hl2FilterBoardSettings::setManualEnabled(invalid, true);
        check(!Hl2FilterBoardSettings::manualEnabled(invalid),
              "a write through an invalid scope does not silently succeed");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_filter_board_settings_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
