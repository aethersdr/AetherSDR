// Verifies the per-model feature-flag table in ModelCapabilities mirrors
// FlexLib/ModelInfo.cs row-for-row for Has4Meters / Has2Meters and
// HasLoopA / HasLoopB (#695).
// Catches drift if FlexLib ever flips a flag — re-sync the C++ table
// and update this test together.

#include "models/ModelCapabilities.h"

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

} // namespace

int main()
{
    int failures = 0;

    // Exact model strings from FlexLib/ModelInfo.cs.  Expected flags
    // copied directly from that file's Has4Meters / Has2Meters /
    // HasLoopA / HasLoopB columns.
    struct Row {
        const char* model;
        bool has4m;
        bool has2m;
        bool hasLoopA;
        bool hasLoopB;
    };
    constexpr Row kExpected[] = {
        {"FLEX-6300",  false, false, false, false},
        {"FLEX-6400",  false, false, false, false},
        {"FLEX-6400M", false, false, false, false},
        {"FLEX-6500",  true,  false, true,  false},
        {"FLEX-6600",  false, false, false, false},
        {"FLEX-6600M", false, false, false, false},
        {"FLEX-6700",  true,  true,  true,  true },
        {"FLEX-6700R", false, false, false, false},
        {"FLEX-8400",  false, false, false, false},
        {"FLEX-8400M", false, false, false, false},
        {"FLEX-8600",  false, false, false, false},
        {"FLEX-8600M", false, false, false, false},
        {"AU-520",     false, false, false, false},
        {"ML-380",     false, false, false, false},
        {"CL-200",     false, false, false, false},
        {"RT-100",     false, false, false, false},
    };

    for (const auto& row : kExpected) {
        const ModelCapabilities caps = capabilitiesFor(QString::fromLatin1(row.model));
        if (!expect(caps.has4Meters == row.has4m,
                    (std::string(row.model) + ": has4Meters").c_str()))
            ++failures;
        if (!expect(caps.has2Meters == row.has2m,
                    (std::string(row.model) + ": has2Meters").c_str()))
            ++failures;
        if (!expect(caps.hasLoopA == row.hasLoopA,
                    (std::string(row.model) + ": hasLoopA").c_str()))
            ++failures;
        if (!expect(caps.hasLoopB == row.hasLoopB,
                    (std::string(row.model) + ": hasLoopB").c_str()))
            ++failures;
    }

    // Substring match — vendor suffixes like "FLEX-6700/A" should still
    // resolve to the 6700 row.
    {
        const auto caps = capabilitiesFor(QStringLiteral("FLEX-6700/A"));
        if (!expect(caps.has4Meters && caps.has2Meters
                        && caps.hasLoopA && caps.hasLoopB,
                    "FLEX-6700/A resolves to 6700 row"))
            ++failures;
    }

    // Case-insensitive — the model string the radio reports is upper-
    // case today but downstream code shouldn't have to rely on that.
    {
        const auto caps = capabilitiesFor(QStringLiteral("flex-6700"));
        if (!expect(caps.has4Meters && caps.has2Meters
                        && caps.hasLoopA && caps.hasLoopB,
                    "lowercase model resolves to 6700 row"))
            ++failures;
    }

    // Unknown model — default-init capabilities, no UI surfaces.
    // Forward-compat for radios released after this build.
    {
        const auto caps = capabilitiesFor(QStringLiteral("FLEX-9999"));
        if (!expect(!caps.has4Meters && !caps.has2Meters
                        && !caps.hasLoopA && !caps.hasLoopB,
                    "Unknown model returns default-false capabilities"))
            ++failures;
    }

    // Empty / disconnected state should not crash and should return
    // default capabilities.
    {
        const auto caps = capabilitiesFor(QString());
        if (!expect(!caps.has4Meters && !caps.has2Meters
                        && !caps.hasLoopA && !caps.hasLoopB,
                    "Empty model string returns default-false capabilities"))
            ++failures;
    }

    if (failures == 0) {
        std::cout << "\nAll model-capability tests passed.\n";
        return 0;
    }
    std::cout << "\n" << failures << " test(s) failed.\n";
    return 1;
}
