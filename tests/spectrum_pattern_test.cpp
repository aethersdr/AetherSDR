// Unit test for the demo-mode SpectrumPatternGenerator (RFC #4288, Phase 2a).
// The generator is a pure function of (pattern, geometry, t, frame): this test
// pins its contract — correct length, clamping into the display range,
// determinism (same inputs -> identical bins, so golden captures repeat), and a
// few pattern-specific shape properties.

#include "core/backends/sim/SpectrumPatternGenerator.h"

#include <QCoreApplication>

#include <algorithm>
#include <cstdio>

using AetherSDR::SpectrumPatternGenerator;
using Pattern = SpectrumPatternGenerator::Pattern;
using Geometry = SpectrumPatternGenerator::Geometry;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

Geometry defaultGeometry()
{
    Geometry g;
    g.bins = 1024;
    g.minDbm = -140.0;
    g.maxDbm = -20.0;
    g.spanHz = 250000.0;
    g.signalWidthHz = 10000.0;
    return g;
}

bool allWithin(const QVector<float>& bins, double lo, double hi)
{
    return std::all_of(bins.cbegin(), bins.cend(),
                       [lo, hi](float v) { return v >= lo && v <= hi; });
}

void testLengthAndClamping()
{
    const Geometry g = defaultGeometry();
    for (const QString& n : SpectrumPatternGenerator::allNames()) {
        const Pattern p = SpectrumPatternGenerator::fromName(n);
        const QVector<float> bins =
            SpectrumPatternGenerator::generate(p, g, 1.5, 30);
        report((n + " has the requested bin count").toUtf8().constData(),
               bins.size() == g.bins);
        report((n + " stays within the display range").toUtf8().constData(),
               allWithin(bins, g.minDbm, g.maxDbm));
    }
}

void testDeterminism()
{
    const Geometry g = defaultGeometry();
    // Even the "noisy" band pattern must reproduce for identical inputs.
    const QVector<float> a =
        SpectrumPatternGenerator::generate(Pattern::NoiseBand, g, 2.0, 42);
    const QVector<float> b =
        SpectrumPatternGenerator::generate(Pattern::NoiseBand, g, 2.0, 42);
    report("same (pattern,geometry,t,frame) yields identical bins", a == b);

    const QVector<float> c =
        SpectrumPatternGenerator::generate(Pattern::NoiseBand, g, 2.0, 43);
    report("a different frame index changes the noise texture", a != c);
}

void testNoiseFloorIsFlat()
{
    const Geometry g = defaultGeometry();
    const QVector<float> bins =
        SpectrumPatternGenerator::generate(Pattern::NoiseFloor, g, 0.0, 0);
    const bool flat = std::all_of(bins.cbegin(), bins.cend(),
                                  [&](float v) { return v == bins.first(); });
    report("noise_floor is a single flat level", flat);
    report("noise_floor sits above the display bottom",
           bins.first() > static_cast<float>(g.minDbm));
}

void testRampRisesOverTime()
{
    const Geometry g = defaultGeometry();
    // Ramp period is 10 s; sample early vs. late in one period.
    const float early =
        SpectrumPatternGenerator::generate(Pattern::Ramp, g, 0.5, 0).first();
    const float late =
        SpectrumPatternGenerator::generate(Pattern::Ramp, g, 9.0, 180).first();
    report("ramp level increases across a period", late > early);
}

void testTwoToneIsSymmetric()
{
    const Geometry g = defaultGeometry();
    const QVector<float> bins =
        SpectrumPatternGenerator::generate(Pattern::TwoTone, g, 0.0, 0);
    const int center = g.bins / 2;
    // Find the two peaks; they must be mirror images about center and equal.
    int loPeak = -1, hiPeak = -1;
    for (int b = 0; b < center; ++b) {
        if (bins[b] > static_cast<float>(g.minDbm + 20.0)) { loPeak = b; }
    }
    for (int b = center; b < g.bins; ++b) {
        if (bins[b] > static_cast<float>(g.minDbm + 20.0) && hiPeak < 0) {
            hiPeak = b;
        }
    }
    report("two_tone has a tone on each side of center",
           loPeak >= 0 && hiPeak >= 0);
    if (loPeak >= 0 && hiPeak >= 0) {
        report("two_tone tones are symmetric about center",
               (center - loPeak) == (hiPeak - center));
        report("two_tone tones are equal level",
               qFuzzyCompare(bins[loPeak], bins[hiPeak]));
    }
}

void testCalTonesLandAtExpectedLevels()
{
    const Geometry g = defaultGeometry();
    const QVector<float> bins =
        SpectrumPatternGenerator::generate(Pattern::CalTones, g, 0.0, 0);
    // The -40 dBm tone sits at 0.8 * bins; it is the loudest and well above floor.
    const int b = static_cast<int>(0.8 * g.bins);
    report("cal_tones places a strong tone at the 0.8 position",
           bins[b] > static_cast<float>(-45.0));
}

void testUnknownNameFailsClosed()
{
    bool ok = true;
    const Pattern p =
        SpectrumPatternGenerator::fromName(QStringLiteral("does_not_exist"), &ok);
    report("an unknown pattern name reports not-ok", !ok);
    report("an unknown pattern name falls back to noise_floor",
           p == Pattern::NoiseFloor);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testLengthAndClamping();
    testDeterminism();
    testNoiseFloorIsFlat();
    testRampRisesOverTime();
    testTwoToneIsSymmetric();
    testCalTonesLandAtExpectedLevels();
    testUnknownNameFailsClosed();

    if (g_failed == 0) {
        std::printf("All SpectrumPatternGenerator checks passed\n");
    }
    return g_failed == 0 ? 0 : 1;
}
