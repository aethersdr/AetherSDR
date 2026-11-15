#include "core/SpotModeResolver.h"

#include <QCoreApplication>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectEqual(const char* name, const QString& got, const QString& want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got \"%s\", want \"%s\"\n",
                    name, qUtf8Printable(got), qUtf8Printable(want));
        ++g_failed;
    }
}

void testExtractFromComment()
{
    using SpotModeResolver::extractSpotModeFromComment;
    expectEqual("RBN: leading mode word",
                extractSpotModeFromComment("CW  6 dB 28 WPM CQ"), "CW");
    expectEqual("POTA/Cluster: trailing mode word",
                extractSpotModeFromComment("JP-1277 Higashimurayama CW"), "CW");
    expectEqual("FT8 trailing",
                extractSpotModeFromComment("Calling CQ FT8"), "FT8");
    expectEqual("comment without mode token",
                extractSpotModeFromComment("nice signal"), QString());
    expectEqual("empty comment",
                extractSpotModeFromComment(""), QString());
    expectEqual("case-insensitive match",
                extractSpotModeFromComment("cq rtty"), "RTTY");
}

void testInferFromBand()
{
    using SpotModeResolver::inferSpotModeFromBand;
    expectEqual("40m CW segment", inferSpotModeFromBand(7.020), "CW");
    expectEqual("20m CW segment", inferSpotModeFromBand(14.030), "CW");
    expectEqual("20m FT8 segment", inferSpotModeFromBand(14.074), "DIGU");
    expectEqual("40m phone segment defaults to LSB", inferSpotModeFromBand(7.200), "LSB");
    expectEqual("20m phone segment defaults to USB", inferSpotModeFromBand(14.250), "USB");
    expectEqual("below 1.8 MHz returns empty", inferSpotModeFromBand(0.500), QString());
}

void testSpotToRadioMap()
{
    using SpotModeResolver::mapSpotModeToRadioMode;
    expectEqual("CW maps to CW", mapSpotModeToRadioMode("CW", 7.020), "CW");
    expectEqual("FT8 maps to DIGU", mapSpotModeToRadioMode("FT8", 14.074), "DIGU");
    expectEqual("RTTY maps to DIGL", mapSpotModeToRadioMode("RTTY", 14.080), "DIGL");
    expectEqual("SSB picks USB above 10 MHz",
                mapSpotModeToRadioMode("SSB", 14.250), "USB");
    expectEqual("SSB picks LSB below 10 MHz",
                mapSpotModeToRadioMode("SSB", 7.200), "LSB");
    expectEqual("unknown mode returns empty",
                mapSpotModeToRadioMode("BLARGH", 14.0), QString());
}

void testResolveSpotRadioMode()
{
    using SpotModeResolver::resolveSpotRadioMode;
    // Issue #2298 motivating case: radio in SSB on 7 MHz, user clicks a CW spot.
    expectEqual("issue 2298: CW spot at 7.030 MHz from comment",
                resolveSpotRadioMode("", "VK3ABC up 1.5 CW", 7.030), "CW");
    expectEqual("issue 2298: CW spot at 7.030 MHz from leading word",
                resolveSpotRadioMode("", "CW 12 dB 25 WPM CQ", 7.030), "CW");
    expectEqual("explicit mode wins over band-plan",
                resolveSpotRadioMode("FT8", "", 7.200), "DIGU");
    expectEqual("explicit mode wins over comment",
                resolveSpotRadioMode("CW", "FT8 calling", 14.080), "CW");
    expectEqual("no explicit mode, no comment hit, falls to band-plan CW",
                resolveSpotRadioMode("", "no hint here", 14.030), "CW");
    expectEqual("no explicit mode, no comment hit, falls to phone-band default",
                resolveSpotRadioMode("", "no hint here", 14.250), "USB");
    expectEqual("nothing resolvable returns empty",
                resolveSpotRadioMode("", "no hint here", 0.0), QString());
    expectEqual("explicit lowercase mode normalised",
                resolveSpotRadioMode("ft8", "", 14.074), "DIGU");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testExtractFromComment();
    testInferFromBand();
    testSpotToRadioMap();
    testResolveSpotRadioMode();

    return g_failed == 0 ? 0 : 1;
}
