// Squelch level -> panadapter scale (#6092). Socket-free: backends are
// constructed cold, nothing connects, and the HL2 spectrum is fed synthetic IQ.
//
//   1. The HL2 amsq gate is referred to the LNA: -108 + 0.7*L - lnaOffsetDb()
//      dBFS, and at level 50 it sits between on8st's measured no-signal and
//      carrier brackets on every LNA row he ran (+20, +8, -4, -12).
//   2. Each backend's RadioCapabilities::squelchLevelScale: Flex -160 + 1*L in
//      every mode with Auto SQL; HL2 its exact pan-axis map in amsq's modes
//      only, without Auto SQL; an unidentified Icom, demo, ANAN and RTL keep
//      Flex's scale (the IC-7300MK2's measured map is in
//      icom_control_profile_test, #6180).
//   3. The line: a carrier exactly at the amsq gate reads on the HL2 pan at
//      the record's threshold, at any LNA gain. Flex's line and Auto SQL level
//      are what the old -160 + level code produced.
//   4. The mode filter: no line or Auto SQL where the record does not apply.

#include "TestSettingsProfile.h"
#include "core/backends/SquelchLevelScale.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2DbReference.h"
#include "core/backends/hl2/Hl2Spectrum.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "core/dsp/WdspChannel.h"
#ifdef AETHER_BACKEND_RTL
#include "core/backends/rtl/RtlSdrBackend.h"
#include "core/backends/rtl/RtlSquelchGate.h"
#endif

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <numbers>
#include <vector>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool cond, const char* what)
{
    std::fprintf(cond ? stdout : stderr, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) {
        ++g_failures;
    }
}

bool nearly(double a, double b, double tol = 1.0e-9)
{
    return std::abs(a - b) <= tol;
}

double gateDbfs(double lnaGainDb, int level)
{
    hl2::Hl2DbReference ref;
    ref.setLnaGainDb(lnaGainDb);
    return WdspChannel::levelSquelchThresholdDbfsForLevel(level, ref.levelSquelchOffsetDb());
}

void hl2GateIsReferredToTheLna()
{
    for (const double gain : {-12.0, -4.0, 8.0, 20.0, 48.0}) {
        hl2::Hl2DbReference ref;
        ref.setLnaGainDb(gain);
        for (const int level : {0, 1, 25, 50, 75, 100}) {
            check(nearly(gateDbfs(gain, level), -108.0 + 0.7 * level - ref.lnaOffsetDb()),
                  "gate is -108 + 0.7*L - lnaOffsetDb() dBFS");
        }
        check(nearly(gateDbfs(gain + 10.0, 50) - gateDbfs(gain, 50), 10.0),
              "the gate moves dB for dB with the LNA");
    }
    check(nearly(gateDbfs(hl2::Hl2DbReference::kLevelSquelchAnchorLnaGainDb, 50), -105.0),
          "at the -12 dB anchor the map is the measured -140 + 0.7*L");

    // on8st's 2026-10-03 rows (#6092), raw dBFS where the gate opened on each
    // input: level 50 must keep the no-signal input closed and open on the
    // carrier at every gain he measured.
    struct Row {
        double lna, noiseTop, carrierBottom;
    };
    const Row rows[] = {
        {20.0, -73.5, -70.0},
        {8.0, -93.8, -75.6},
        {-4.0, -100.8, -84.7},
        {-12.0, -109.9, -93.1},
    };
    for (const Row& r : rows) {
        const double gate = gateDbfs(r.lna, 50);
        std::printf("  LNA %+.0f dB: level 50 gate %.1f dBFS (noise top %.1f, carrier %.1f)\n",
                    r.lna, gate, r.noiseTop, r.carrierBottom);
        check(gate > r.noiseTop && gate < r.carrierBottom,
              "level 50 sits between on8st's no-signal and carrier brackets");
    }
}

double hl2RecordOffset(double lnaGainDb)
{
    hl2::Hl2DbReference ref;
    ref.setLnaGainDb(lnaGainDb);
    return WdspChannel::kLevelSquelchBaseDbfs + ref.levelSquelchOffsetDb() + ref.offsetDb()
           + hl2::Hl2Spectrum::kToneGainDb;
}

void recordsPerBackend()
{
    const RadioCapabilities flex = FlexBackend().capabilities();
    check(flex.squelchLevelScale.has_value(), "Flex publishes a squelch scale");
    if (flex.squelchLevelScale) {
        check(*flex.squelchLevelScale == SquelchLevelScale{-160.0, 1.0, {}, true},
              "Flex: -160 + 1*level in every mode, with Auto SQL");
    }

    const RadioCapabilities hl2caps = hl2::Hl2Backend().capabilities();
    check(hl2caps.squelchLevelScale.has_value(), "HL2 publishes a squelch scale");
    if (hl2caps.squelchLevelScale) {
        const SquelchLevelScale& s = *hl2caps.squelchLevelScale;
        check(nearly(s.dbPerStep, 0.7), "HL2: 0.7 dB per level step");
        check(nearly(s.offsetDb, hl2RecordOffset(hl2::Hl2DbReference::kDefaultLnaGainDb)),
              "HL2: offset is amsq base + LNA referral + pan offset + tone gain");
        check(nearly(s.offsetDb, -125.0 + hl2::Hl2Spectrum::kToneGainDb),
              "HL2: offset is -125 dB + the carrier term with a +3 dBm full scale");
        // Set membership is the contract (appliesTo() uses contains()).
        QStringList modes = s.modes;
        modes.sort();
        check(modes == QStringList({QStringLiteral("AM"), QStringLiteral("DSB"),
                                    QStringLiteral("LSB"), QStringLiteral("SAM"),
                                    QStringLiteral("USB")}),
              "HL2: amsq's modes only (no FM, CW or data)");
        check(!s.autoSquelch, "HL2: no Auto SQL");
    }
    for (const double gain : {-12.0, 0.0, 20.0, 48.0}) {
        check(nearly(hl2RecordOffset(gain), hl2RecordOffset(20.0)),
              "HL2: the pan-axis offset does not move with the LNA");
    }

    // No measured map of their own: Flex's scale, so nothing changes for them.
    const SquelchLevelScale legacy{-160.0, 1.0, {}, true};
    check(legacyDbmSquelchScale() == legacy, "the legacy scale is -160 + 1*level with Auto SQL");
    check(icom::IcomCivBackend().capabilities().squelchLevelScale == legacy,
          "unidentified Icom: Flex's -160 + 1*level, every mode, with Auto SQL");
    check(SimBackend().capabilities().squelchLevelScale == legacy,
          "Demo: Flex's -160 + 1*level, every mode, with Auto SQL");
    check(anan::AnanBackend().capabilities().squelchLevelScale == legacy,
          "ANAN: Flex's -160 + 1*level, every mode, with Auto SQL");
#ifdef AETHER_BACKEND_RTL
    {
        // RtlSquelchGate is the receiver's own gate: its map, FM/FMN only, Auto
        // SQL (the gate reads the pan's FFT), and SQL nowhere else.
        const SquelchLevelScale rtlScale{rtl::RtlSquelchGate::kReferenceDb,
            rtl::RtlSquelchGate::kStepDb, {QStringLiteral("FM"), QStringLiteral("FMN")},
            true, QStringLiteral("dBFS/bin"), true};
        check(rtl::RtlSdrBackend().capabilities().squelchLevelScale == rtlScale,
              "RTL-SDR: RtlSquelchGate's -120 + 1.2*level dBFS/bin, FM/FMN, exclusive");
    }
    // Only RTL owns its gate; every other family keeps the #2504 mode rule.
    check(!legacy.modesExclusive && !exclusiveSquelchScale(std::optional(legacy)),
          "the legacy scale is a line placement, not the receiver's gate");
#endif
}

// A steady carrier of magnitude A at an exact FFT bin.
std::vector<std::complex<float>> carrier(int n, int bin, double amplitude)
{
    std::vector<std::complex<float>> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * std::numbers::pi * bin * i / n;
        v[static_cast<std::size_t>(i)] = std::complex<float>(
            static_cast<float>(amplitude * std::cos(ph)),
            static_cast<float>(amplitude * std::sin(ph)));
    }
    return v;
}

double panPeakDbfs(double amplitude)
{
    constexpr int kN = 1024;
    hl2::Hl2Spectrum spectrum(kN);
    std::vector<float> bins;
    spectrum.process(carrier(kN, 100, amplitude), bins);
    return *std::max_element(bins.begin(), bins.end());
}

void lineFromTheRecord()
{
    check(nearly(panPeakDbfs(0.01), -40.0 + hl2::Hl2Spectrum::kToneGainDb, 0.01),
          "a carrier of magnitude A reads 20*log10(A) + kToneGainDb on the HL2 pan");

    const SquelchLevelScale hl2Scale = *hl2::Hl2Backend().capabilities().squelchLevelScale;
    for (const double gain : {-12.0, 8.0, 20.0}) {
        hl2::Hl2DbReference ref;
        ref.setLnaGainDb(gain);
        for (const int level : {20, 50, 80}) {
            // A carrier exactly at the gate, as the pan displays it.
            const double gate = gateDbfs(gain, level);
            const double displayed = panPeakDbfs(std::pow(10.0, gate / 20.0)) + ref.offsetDb();
            check(nearly(displayed, hl2Scale.thresholdDb(level), 0.01),
                  "HL2: a carrier at the amsq gate peaks on the drawn line");
        }
    }

    const SquelchLevelScale flex{-160.0, 1.0, {}, true};
    check(nearly(flex.thresholdDb(50), -110.0), "Flex: level 50 draws at -110 dBm");
    bool same = true;
    for (float target = -175.0f; target <= -40.0f; target += 0.37f) {
        const int old = std::clamp(static_cast<int>(target - -160.0f + 0.5f), 1, 100);
        same = same && flex.levelForThresholdDb(target) == old;
    }
    check(same, "Flex: Auto SQL picks the level the -160 + level code did");
    check(flex.levelForThresholdDb(1.0e9) == 100 && flex.levelForThresholdDb(-1.0e9) == 1,
          "Auto SQL levels clamp to 1..100");
}

void modeFilter()
{
    const auto hl2Scale = hl2::Hl2Backend().capabilities().squelchLevelScale;
    check(squelchScaleForMode(hl2Scale, QStringLiteral("USB")).has_value(),
          "HL2 USB: the line is drawn");
    check(squelchScaleForMode(hl2Scale, QStringLiteral("am")).has_value(),
          "the mode match ignores case");
    check(!squelchScaleForMode(hl2Scale, QStringLiteral("FM")), "HL2 FM: no line (fmsq)");
    check(!squelchScaleForMode(hl2Scale, QStringLiteral("CW")), "HL2 CW: no line");
    check(!autoSquelchAvailable(squelchScaleForMode(hl2Scale, QStringLiteral("USB"))),
          "HL2 USB: no Auto SQL");
    check(!squelchScaleForMode(std::nullopt, QStringLiteral("USB")),
          "absent record: no line in any mode");
    check(!autoSquelchAvailable(std::nullopt), "absent record: no Auto SQL");
    const std::optional<SquelchLevelScale> flex = SquelchLevelScale{-160.0, 1.0, {}, true};
    check(autoSquelchAvailable(squelchScaleForMode(flex, QStringLiteral("FM"))),
          "Flex: line and Auto SQL in every mode");
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("squelch-level-scale-test"));
    QCoreApplication app(argc, argv);
    hl2GateIsReferredToTheLna();
    recordsPerBackend();
    lineFromTheRecord();
    modeFilter();
    std::printf(g_failures == 0 ? "squelch_level_scale_test: all passed\n"
                                : "squelch_level_scale_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
