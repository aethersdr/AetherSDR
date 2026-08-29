// Unit tests for KiwiSDR DX Community spots loading and parsing.

#include "models/BandPlanManager.h"
#include "TestSettingsProfile.h"

#include <QCoreApplication>
#include <QString>
#include <QVector>

#include <cmath>
#include <iostream>

using namespace AetherSDR;

namespace {

int g_pass = 0;
int g_fail = 0;

bool expect(bool condition, const char* label)
{
    if (condition) {
        std::cout << "[ OK ] " << label << '\n';
        ++g_pass;
    } else {
        std::cout << "[FAIL] " << label << '\n';
        ++g_fail;
    }
    return condition;
}

} // namespace

int main(int argc, char* argv[])
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-kiwisdr-dx-spots-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);

    BandPlanManager mgr;
    mgr.loadKiwiDxSpots();

    const auto& spots = mgr.kiwiDxSpots();

    expect(!spots.isEmpty(), "KiwiSDR DX Community spots loaded from Qt resources");
    expect(spots.size() > 500, "KiwiSDR DX Community spot count > 500 entries");

    // Test specific entry parsing
    bool foundAlpha = false;
    bool foundGrimeton = false;
    bool foundWideband = false;

    for (const auto& spot : spots) {
        if (spot.name == "Alpha F1 RUS") {
            foundAlpha = true;
            expect(std::abs(spot.freqMhz - 0.0119) < 1e-5, "Alpha F1 RUS frequency conversion kHz to MHz (11.9 kHz -> 0.0119 MHz)");
            expect(spot.mode == "CWN", "Alpha F1 RUS mode is CWN");
            expect(spot.notes == "navigation system", "Alpha F1 RUS notes field parsed");
        }
        if (spot.name == "SAQ SWE") {
            foundGrimeton = true;
            expect(std::abs(spot.freqMhz - 0.0172) < 1e-5, "SAQ SWE frequency is 0.0172 MHz");
            expect(spot.notes.contains("Grimeton alternator"), "SAQ SWE notes contain Grimeton alternator");
            expect(spot.notes.contains("\n"), "SAQ SWE notes contain multiline newline");
        }
        if (spot.name == "Alpha wideband") {
            foundWideband = true;
            expect(spot.loOffsetHz == -3500, "Alpha wideband lo passband offset is -3500 Hz");
            expect(spot.hiOffsetHz == -300, "Alpha wideband hi passband offset is -300 Hz");
        }
    }

    expect(foundAlpha, "Alpha F1 RUS entry found in loaded spots");
    expect(foundGrimeton, "SAQ SWE entry found in loaded spots");
    expect(foundWideband, "Alpha wideband entry with passband boundaries found");

    // Test spot mode normalization / whitelist
    expect(BandPlanManager::normalizeSpotMode("CWN") == "CW", "normalizeSpotMode converts CWN to CW");
    expect(BandPlanManager::normalizeSpotMode("AMN") == "AM", "normalizeSpotMode converts AMN to AM");
    expect(BandPlanManager::normalizeSpotMode("DRM") == "AM", "normalizeSpotMode converts DRM to AM");
    expect(BandPlanManager::normalizeSpotMode("SAL") == "SAM", "normalizeSpotMode converts SAL to SAM");
    expect(BandPlanManager::normalizeSpotMode("SAU") == "SAM", "normalizeSpotMode converts SAU to SAM");
    expect(BandPlanManager::normalizeSpotMode("NBFM") == "FM", "normalizeSpotMode converts NBFM to FM");
    expect(BandPlanManager::normalizeSpotMode("USB") == "USB", "normalizeSpotMode preserves valid mode USB");
    expect(BandPlanManager::normalizeSpotMode("IQ").isEmpty(), "normalizeSpotMode returns empty for unsupported mode IQ");
    expect(BandPlanManager::normalizeSpotMode("GARBAGE").isEmpty(), "normalizeSpotMode returns empty for unknown mode GARBAGE");

    std::cout << "Summary: " << g_pass << " passed, " << g_fail << " failed.\n";
    return (g_fail == 0) ? 0 : 1;
}
