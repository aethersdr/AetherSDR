// Verifies the per-model feature-flag table in ModelCapabilities mirrors
// FlexLib/ModelInfo.cs row-for-row for Platform, Has4Meters / Has2Meters
// and HasLoopA / HasLoopB (#695, #2177).
// Catches drift if FlexLib ever flips a flag — re-sync the C++ table
// and update this test together.

#include "models/ModelCapabilities.h"

#include <iostream>
#include <string>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const std::string& label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

const char* platformName(RadioPlatform p)
{
    switch (p) {
        case RadioPlatform::Unknown:    return "Unknown";
        case RadioPlatform::Microburst: return "Microburst";
        case RadioPlatform::DeepEddy:   return "DeepEddy";
        case RadioPlatform::BigBend:    return "BigBend";
        case RadioPlatform::DragonFire: return "DragonFire";
    }
    return "?";
}

} // namespace

int main()
{
    int failures = 0;
    auto check = [&](bool cond, const std::string& label) {
        if (!expect(cond, label)) ++failures;
    };

    // Exact model strings from FlexLib/ModelInfo.cs.  Expected columns
    // copied directly from that file: Platform / Has4Meters / Has2Meters /
    // HasLoopA / HasLoopB.  hasExtendedDsp is derived (BigBend|DragonFire).
    struct Row {
        const char* model;
        RadioPlatform platform;
        bool has4m;
        bool has2m;
        bool hasLoopA;
        bool hasLoopB;
        bool diversity;
        int  slices;   // FlexLib SliceList size == max slices == max panadapters
    };
    const Row kExpected[] = {
        {"FLEX-6300",  RadioPlatform::Microburst, false, false, false, false, false, 2},
        {"FLEX-6400",  RadioPlatform::DeepEddy,   false, false, false, false, false, 2},
        {"FLEX-6400M", RadioPlatform::DeepEddy,   false, false, false, false, false, 2},
        {"FLEX-6500",  RadioPlatform::Microburst, true,  false, true,  false, false, 4},
        {"FLEX-6600",  RadioPlatform::DeepEddy,   false, false, false, false, true,  4},
        {"FLEX-6600M", RadioPlatform::DeepEddy,   false, false, false, false, true,  4},
        {"FLEX-6700",  RadioPlatform::Microburst, true,  true,  true,  true,  true,  8},
        {"FLEX-6700R", RadioPlatform::Microburst, false, false, false, false, true,  8},
        {"FLEX-8400",  RadioPlatform::BigBend,    false, false, false, false, false, 2},
        {"FLEX-8400M", RadioPlatform::BigBend,    false, false, false, false, false, 2},
        {"FLEX-8600",  RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"FLEX-8600M", RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"ML-9600",    RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"ML-9600W",   RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"ML-9600X",   RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"MLS-9601",   RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"CL-9300",    RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"CLS-9301",   RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"RT-2122",    RadioPlatform::DragonFire, false, false, false, false, false, 2},
        {"AU-510",     RadioPlatform::BigBend,    false, false, false, false, false, 2},
        {"AU-510M",    RadioPlatform::BigBend,    false, false, false, false, false, 2},
        {"AU-520",     RadioPlatform::BigBend,    false, false, false, false, true,  4},
        {"AU-520M",    RadioPlatform::BigBend,    false, false, false, false, true,  4},
    };

    for (const auto& row : kExpected) {
        const ModelCapabilities caps = capabilitiesFor(QString::fromLatin1(row.model));
        const std::string m = row.model;
        check(caps.platform == row.platform,
              m + ": platform == " + platformName(row.platform)
                  + " (got " + platformName(caps.platform) + ")");
        check(caps.has4Meters == row.has4m,  m + ": has4Meters");
        check(caps.has2Meters == row.has2m,  m + ": has2Meters");
        check(caps.hasLoopA == row.hasLoopA, m + ": hasLoopA");
        check(caps.hasLoopB == row.hasLoopB, m + ": hasLoopB");
        check(caps.isDiversityAllowed == row.diversity, m + ": isDiversityAllowed");
        check(caps.maxSlices == row.slices,
              m + ": maxSlices == " + std::to_string(row.slices)
                  + " (got " + std::to_string(caps.maxSlices) + ")");
        // Extended firmware DSP (NRL/NRS/RNN/NRF) = BigBend | DragonFire.
        const bool expectExt = row.platform == RadioPlatform::BigBend
                            || row.platform == RadioPlatform::DragonFire;
        check(caps.hasExtendedDsp() == expectExt, m + ": hasExtendedDsp");
    }

    // Regression: the dual-SCU ML-/MLS-/CL-/CLS- models must report 4 slices/
    // panadapters (SliceList {A,B,C,D}); the old maxPanadapters() contains()
    // list omitted them and capped them at 2. AU-510 (2) vs AU-520 (4) confirm
    // the M-variant families resolve to the right capacity.
    for (const char* m : {"ML-9600", "MLS-9601", "CL-9300", "CLS-9301"}) {
        check(capabilitiesFor(QString::fromLatin1(m)).maxSlices == 4,
              std::string(m) + " reports 4 slices/pans (was capped at 2)");
    }
    check(capabilitiesFor(QStringLiteral("AU-510M")).maxSlices == 2,
          "AU-510M reports 2 slices/pans");
    check(capabilitiesFor(QStringLiteral("AU-520M")).maxSlices == 4,
          "AU-520M reports 4 slices/pans");

    // Regression: the "S" server variants must NOT be lost the way the old
    // contains("ML-")/contains("CL-") prefix logic dropped them (MLS-9601 has
    // no "ML-" substring; CLS-9301 has no "CL-").
    check(capabilitiesFor(QStringLiteral("MLS-9601")).hasExtendedDsp(),
          "MLS-9601 reports extended DSP (was missed by contains(\"ML-\"))");
    check(capabilitiesFor(QStringLiteral("CLS-9301")).hasExtendedDsp(),
          "CLS-9301 reports extended DSP (was missed by contains(\"CL-\"))");

    // Regression: the AU-510 must report extended DSP regardless of case —
    // the reported user symptom.  FlexLib matches exact upper-case, but the
    // gate must not depend on that.
    check(capabilitiesFor(QStringLiteral("AU-510")).hasExtendedDsp(),
          "AU-510 reports extended DSP");
    check(capabilitiesFor(QStringLiteral("au-510")).hasExtendedDsp(),
          "au-510 (lowercase) reports extended DSP");
    check(capabilitiesFor(QStringLiteral("AU-510M")).hasExtendedDsp(),
          "AU-510M reports extended DSP");

    // 6000-series (Microburst/DeepEddy) must NOT report extended DSP.
    for (const char* m : {"FLEX-6300", "FLEX-6400", "FLEX-6500",
                          "FLEX-6600", "FLEX-6700", "FLEX-6700R"}) {
        check(!capabilitiesFor(QString::fromLatin1(m)).hasExtendedDsp(),
              std::string(m) + " does NOT report extended DSP");
    }

    // Diversity regressions vs the old contains() gate:
    //  - FLEX-6500 is single-SCU -> diversity must be FALSE (old gate wrongly
    //    listed contains("6500") as diversity-allowed).
    check(!capabilitiesFor(QStringLiteral("FLEX-6500")).isDiversityAllowed,
          "FLEX-6500 diversity is FALSE (old gate false-positive)");
    //  - the ML-/MLS-/CL-/CLS- dual-SCU models are diversity-allowed but the
    //    old gate never listed them.
    for (const char* m : {"ML-9600", "ML-9600W", "MLS-9601", "CL-9300", "CLS-9301"}) {
        check(capabilitiesFor(QString::fromLatin1(m)).isDiversityAllowed,
              std::string(m) + " diversity is TRUE (old gate false-negative)");
    }
    //  - RT-2122 (DragonFire, single-SCU) is NOT diversity-allowed.
    check(!capabilitiesFor(QStringLiteral("RT-2122")).isDiversityAllowed,
          "RT-2122 diversity is FALSE");

    // Substring match — vendor suffixes like "FLEX-6700/A" resolve to 6700.
    {
        const auto caps = capabilitiesFor(QStringLiteral("FLEX-6700/A"));
        check(caps.has4Meters && caps.has2Meters
                  && caps.hasLoopA && caps.hasLoopB,
              "FLEX-6700/A resolves to 6700 row");
    }

    // Case-insensitive — the model string is upper-case today but downstream
    // code shouldn't rely on that.
    {
        const auto caps = capabilitiesFor(QStringLiteral("flex-6700"));
        check(caps.has4Meters && caps.has2Meters
                  && caps.hasLoopA && caps.hasLoopB,
              "lowercase model resolves to 6700 row");
    }

    // Unknown model — Unknown platform, all-false, no extended DSP.
    // Forward-compat for radios released after this build.
    {
        const auto caps = capabilitiesFor(QStringLiteral("FLEX-9999"));
        check(caps.platform == RadioPlatform::Unknown
                  && !caps.has4Meters && !caps.has2Meters
                  && !caps.hasLoopA && !caps.hasLoopB
                  && !caps.hasExtendedDsp()
                  && caps.maxSlices == 2,   // FlexLib DEFAULT SliceList {A,B}
              "Unknown model returns default Unknown/all-false/2-slice capabilities");
    }

    // Empty / disconnected state should not crash and should return default
    // (Unknown) capabilities — no extended DSP before the model is known
    // (avoids a pre-discovery button flash).
    {
        const auto caps = capabilitiesFor(QString());
        check(caps.platform == RadioPlatform::Unknown && !caps.hasExtendedDsp(),
              "Empty model string returns Unknown, no extended DSP");
    }

    if (failures == 0) {
        std::cout << "\nAll model-capability tests passed.\n";
        return 0;
    }
    std::cout << "\n" << failures << " test(s) failed.\n";
    return 1;
}
