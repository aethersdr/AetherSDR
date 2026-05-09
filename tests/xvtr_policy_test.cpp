// Standalone regression tests for XVTR band-stack keys and waterfall IF/RF
// mapping. Build target: xvtr_policy_test.

#include "models/XvtrPolicy.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-72s %s\n",
                ok ? "[ OK ]" : "[FAIL]",
                name,
                detail.c_str());
    if (!ok) ++g_failed;
}

bool near(double a, double b, double eps = 1e-9)
{
    return std::abs(a - b) <= eps;
}

XvtrPolicy::Transverter xvtr(int index, int order, const char* name,
                             double rfMhz, double ifMhz, bool valid = true)
{
    return {index, order, QString::fromLatin1(name), rfMhz, ifMhz, valid};
}

std::string detailFor(const XvtrPolicy::WaterfallTileRange& range)
{
    return "low=" + std::to_string(range.lowMhz)
        + " high=" + std::to_string(range.highMhz)
        + " shifted=" + std::to_string(range.shifted);
}

void expectBandKey(const char* label, const QString& bandName,
                   const QVector<XvtrPolicy::Transverter>& xvtrs,
                   const QString& expectedKey)
{
    const auto result = XvtrPolicy::resolveBandStackKey(bandName, xvtrs);
    report(label,
           result.isSupported() && result.key == expectedKey,
           QStringLiteral("key=%1 reason=%2")
               .arg(result.key, result.unsupportedReason)
               .toStdString());
}

void expectUnsupportedBand(const char* label, const QString& bandName,
                           const QVector<XvtrPolicy::Transverter>& xvtrs,
                           const QString& expectedReasonFragment)
{
    const auto result = XvtrPolicy::resolveBandStackKey(bandName, xvtrs);
    report(label,
           !result.isSupported() &&
               result.unsupportedReason.contains(expectedReasonFragment),
           QStringLiteral("key=%1 reason=%2")
               .arg(result.key, result.unsupportedReason)
               .toStdString());
}

void expectRange(const char* label, const XvtrPolicy::WaterfallTileRange& range,
                 double expectedLow, double expectedHigh, bool expectedShifted)
{
    report(label,
           near(range.lowMhz, expectedLow) &&
               near(range.highMhz, expectedHigh) &&
               range.shifted == expectedShifted,
           detailFor(range));
}

void testBandStackKeysUseFlexIndex()
{
    const QVector<XvtrPolicy::Transverter> xvtrs = {
        xvtr(12, 3, "2m",    144.0, 28.0),
        xvtr(4,  7, "70cm",  432.0, 28.0),
        xvtr(1,  9, "1.2G", 1296.0, 28.0),
    };

    expectBandKey("native HF strips UI suffix", "20m", xvtrs, "20");
    expectBandKey("native utility slot WWV", "WWV", xvtrs, "33");
    expectBandKey("native utility slot GEN", "GEN", xvtrs, "34");
    expectBandKey("XVTR 2m uses status index, not setup order", "2m", xvtrs, "X12");
    expectBandKey("XVTR 70cm uses status index, not RF frequency", "70cm", xvtrs, "X4");
    expectBandKey("XVTR 1.2G uses status index", "1.2G", xvtrs, "X1");
}

void testBandStackKeysRefuseGuesses()
{
    const QVector<XvtrPolicy::Transverter> xvtrs = {
        xvtr(2, -1, "2m", 144.0, 28.0),
        xvtr(3,  5, "70cm", 432.0, 28.0, false),
    };

    // order=-1 is irrelevant now — index is always valid; key is X2
    expectBandKey("XVTR with no order field uses index",
                  "2m", xvtrs, "X2");
    expectUnsupportedBand("invalid XVTR entry is ignored",
                          "70cm", xvtrs, "has no Flex display pan band= mapping");
    expectUnsupportedBand("unknown band does not use freq/mode fallback",
                          "1.2G", xvtrs, "has no Flex display pan band= mapping");
}

void testHfWaterfallDoesNotShiftWhenTileLagsByOneSpan()
{
    const QVector<XvtrPolicy::Transverter> xvtrs = {
        xvtr(12, 3, "2m", 144.0, 28.0),
    };

    const auto boundary = XvtrPolicy::mapWaterfallTileRange(
        16.150, 16.650, 16.900, xvtrs, false);
    expectRange("HF tile at one 500 kHz span is unchanged",
                boundary, 16.150, 16.650, false);

    const auto beyond = XvtrPolicy::mapWaterfallTileRange(
        16.150, 16.650, 16.901, xvtrs, false);
    expectRange("HF tile beyond one 500 kHz span is still unchanged",
                beyond, 16.150, 16.650, false);
}

void testXvtrWaterfallMapsIfToRfBands()
{
    {
        const QVector<XvtrPolicy::Transverter> xvtrs = {
            xvtr(12, 3, "2m", 144.0, 28.0),
        };
        const auto match = XvtrPolicy::matchWaterfallTileTransverterOffset(
            28.125, 28.625, 144.375, xvtrs);
        report("2m XVTR offset match reports diagnostic details",
               match.matched &&
                   match.index == 12 &&
                   match.order == 3 &&
                   match.name == "2m" &&
                   near(match.observedOffsetMhz, 116.0) &&
                   near(match.expectedOffsetMhz, 116.0) &&
                   near(match.toleranceMhz, 0.5),
               QStringLiteral("matched=%1 idx=%2 order=%3 observed=%4 expected=%5 tolerance=%6")
                   .arg(match.matched)
                   .arg(match.index)
                   .arg(match.order)
                   .arg(match.observedOffsetMhz)
                   .arg(match.expectedOffsetMhz)
                   .arg(match.toleranceMhz)
                   .toStdString());

        const auto mapped = XvtrPolicy::mapWaterfallTileRange(
            28.125, 28.625, 144.375, xvtrs, false);
        expectRange("2m XVTR maps IF waterfall range into RF range",
                    mapped, 144.125, 144.625, true);
        report("2m XVTR mapped range stays inside 144-148 MHz",
               mapped.lowMhz >= 144.0 && mapped.highMhz <= 148.0,
               detailFor(mapped));
    }

    {
        const QVector<XvtrPolicy::Transverter> xvtrs = {
            xvtr(4, 7, "70cm", 432.0, 28.0),
        };
        const auto mapped = XvtrPolicy::mapWaterfallTileRange(
            28.200, 28.700, 432.450, xvtrs, false);
        expectRange("70cm XVTR maps IF waterfall range into RF range",
                    mapped, 432.200, 432.700, true);
        report("70cm XVTR mapped range stays inside 420-450 MHz",
               mapped.lowMhz >= 420.0 && mapped.highMhz <= 450.0,
               detailFor(mapped));
    }

    {
        const QVector<XvtrPolicy::Transverter> xvtrs = {
            xvtr(1, 9, "1.2G", 1296.0, 28.0),
        };
        const auto mapped = XvtrPolicy::mapWaterfallTileRange(
            28.050, 28.550, 1296.300, xvtrs, false);
        expectRange("1.2 GHz XVTR maps IF waterfall range into RF range",
                    mapped, 1296.050, 1296.550, true);
        report("1.2 GHz XVTR mapped range stays inside 1240-1300 MHz",
               mapped.lowMhz >= 1240.0 && mapped.highMhz <= 1300.0,
               detailFor(mapped));
    }
}

void testXvtrWaterfallGuardrails()
{
    const QVector<XvtrPolicy::Transverter> invalidXvtr = {
        xvtr(12, 3, "2m", 144.0, 28.0, false),
    };

    const auto invalid = XvtrPolicy::mapWaterfallTileRange(
        28.125, 28.625, 144.375, invalidXvtr, false);
    expectRange("invalid XVTR does not trigger IF/RF waterfall shift",
                invalid, 28.125, 28.625, false);

    const auto antennaFallback = XvtrPolicy::mapWaterfallTileRange(
        28.125, 28.625, 144.375, {}, true);
    expectRange("XVT slice antenna can authorize IF/RF waterfall shift",
                antennaFallback, 144.125, 144.625, true);

    const auto inRange = XvtrPolicy::mapWaterfallTileRange(
        144.125, 144.625, 144.375, {}, true);
    expectRange("XVT slice antenna does not shift already-in-range rows",
                inRange, 144.125, 144.625, false);
}

} // namespace

int main()
{
    testBandStackKeysUseFlexIndex();
    testBandStackKeysRefuseGuesses();
    testHfWaterfallDoesNotShiftWhenTileLagsByOneSpan();
    testXvtrWaterfallMapsIfToRfBands();
    testXvtrWaterfallGuardrails();

    return g_failed == 0 ? 0 : 1;
}
