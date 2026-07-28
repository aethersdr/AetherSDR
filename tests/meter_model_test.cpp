#include "models/MeterModel.h"

#include <QCoreApplication>
#include <QVector>

#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

qint16 rawDb(float db)
{
    return static_cast<qint16>(std::lround(db * 128.0f));
}

MeterDef txMeter(int index, const QString& name, const QString& unit = QStringLiteral("dB"),
                 int sourceIndex = 8)
{
    MeterDef def;
    def.index = index;
    def.source = "TX-";
    def.sourceIndex = sourceIndex;
    def.name = name;
    def.unit = unit;
    def.low = 0.0;
    def.high = 25.0;
    return def;
}

MeterDef slcMeter(int index, int sliceIndex)
{
    MeterDef def;
    def.index = index;
    def.source = "SLC";
    def.sourceIndex = sliceIndex;
    def.name = "LEVEL";
    def.unit = "dBm";
    def.low = -150.0;
    def.high = 20.0;
    return def;
}

// These tests keep active-slice routing and direct COMPPEAK coverage. They
// intentionally do not preserve the old AFTEREQ/SC_MIC derivation cases:
// adjacent TX audio meters are diagnostics only and must not synthesize
// compression when COMPPEAK is absent.
void testAdjacentMetersDoNotSynthesizeCompression()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(22, "SC_MIC", "dBFS"));
    model.defineMeter(txMeter(27, "AFTEREQ", "dBFS"));
    model.setActiveTxSlice(0);

    model.updateValues({22, 27}, {rawDb(-10.0f), rawDb(-12.0f)});

    report("adjacent TX audio meters do not synthesize compression",
           !model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f));
}

void testCompPeakDirectlyExposesCompression()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(28, "COMPPEAK"));
    model.setActiveTxSlice(0);

    model.updateValues({28}, {rawDb(12.5f)});

    report("COMPPEAK directly exposes radio compression",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 12.5f));
}

void testCompPeakClampsToGaugeRange()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(28, "COMPPEAK"));
    model.setActiveTxSlice(0);

    model.updateValues({28}, {rawDb(40.0f)});
    const bool clampsHigh = model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 25.0f);

    model.updateValues({28}, {rawDb(-6.0f)});
    const bool clampsLow = model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f);

    report("direct COMPPEAK clamps to the compression gauge range",
           clampsHigh && clampsLow);
}

void testActiveTxSliceSelectsCompPeak()
{
    MeterModel model;
    model.defineMeter(slcMeter(15, 0));
    model.defineMeter(txMeter(23, "COMPPEAK", "dB", 8));
    model.defineMeter(slcMeter(37, 1));
    model.defineMeter(txMeter(45, "COMPPEAK", "dB", 9));

    model.setActiveTxSlice(0);
    model.updateValues({23}, {rawDb(12.0f)});
    report("active TX slice 0 uses its COMPPEAK meter",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 12.0f));

    model.updateValues({45}, {rawDb(20.0f)});
    report("inactive COMPPEAK meter is ignored",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 12.0f));

    model.setActiveTxSlice(1);
    report("changing active TX slice clears stale compression",
           !model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f));

    model.updateValues({45}, {rawDb(20.0f)});
    report("active TX slice 1 uses its COMPPEAK meter",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 20.0f));
}

void testZeroSourceCompPeakUsesSliceContext()
{
    MeterModel model;
    model.defineMeter(slcMeter(14, 0));
    model.defineMeter(txMeter(20, "COMPPEAK", "dB", 0));
    model.defineMeter(slcMeter(32, 1));
    model.defineMeter(txMeter(44, "COMPPEAK", "dB", 0));

    model.setActiveTxSlice(1);
    model.updateValues({20}, {rawDb(8.0f)});
    report("inactive zero-source COMPPEAK meter is ignored",
           !model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f));

    model.updateValues({44}, {rawDb(15.0f)});
    report("zero-source COMPPEAK meter follows active slice context",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 15.0f));
}

void testSparseSliceIdsUseManifestDerivedWaveformBase()
{
    MeterModel model;
    model.defineMeter(slcMeter(37, 1));
    model.defineMeter(txMeter(45, "COMPPEAK", "dB", 9));

    model.setActiveTxSlice(1);
    model.updateValues({45}, {rawDb(18.0f)});

    report("sparse slice IDs use manifest-derived TX waveform base",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 18.0f));
}

void testAfterEqAndScMicDoNotAffectCompression()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(22, "SC_MIC", "dBFS"));
    model.defineMeter(txMeter(27, "AFTEREQ", "dBFS"));
    model.defineMeter(txMeter(28, "COMPPEAK"));
    model.setActiveTxSlice(0);

    model.updateValues({22, 27, 28}, {rawDb(-80.0f), rawDb(-40.0f), rawDb(7.0f)});

    report("AFTEREQ and SC_MIC do not derive or override direct COMPPEAK",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 7.0f));
}

void testRemovingCompPeakMarksCompressionUnavailable()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(28, "COMPPEAK"));
    model.setActiveTxSlice(0);

    model.updateValues({28}, {rawDb(10.0f)});
    model.removeMeter(28);

    report("removing COMPPEAK marks compression unavailable",
           !model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f));
}

void testRemovingAdjacentMetersDoesNotClearCompPeak()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(22, "SC_MIC", "dBFS"));
    model.defineMeter(txMeter(27, "AFTEREQ", "dBFS"));
    model.defineMeter(txMeter(28, "COMPPEAK"));
    model.setActiveTxSlice(0);

    model.updateValues({28}, {rawDb(11.0f)});
    model.removeMeter(22);
    model.removeMeter(27);

    report("removing adjacent TX audio meters does not clear COMPPEAK",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 11.0f));
}

void testDirectionalPowerUsesDirectReflectedMeter()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(9, "REFPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    bool emitted = false;
    bool reflectedPowerMeasured = false;
    float forwardWatts = 0.0f;
    float reflectedWatts = 0.0f;
    float swr = 0.0f;
    QObject::connect(&model, &MeterModel::directionalPowerMetersChanged,
                     [&emitted, &forwardWatts, &reflectedWatts, &swr,
                      &reflectedPowerMeasured](float forward, float reflected,
                                               float ratio, bool measured) {
        emitted = true;
        forwardWatts = forward;
        reflectedWatts = reflected;
        swr = ratio;
        reflectedPowerMeasured = measured;
    });

    model.updateValues({8, 9, 10},
                       {rawDb(50.0f), rawDb(36.0206f), rawDb(1.5f)});

    report("REFPWR is converted from dBm to measured watts",
           emitted && reflectedPowerMeasured
               && nearlyEqual(forwardWatts, 100.0f)
               && nearlyEqual(reflectedWatts, 4.0f)
               && nearlyEqual(model.reflectedPower(), 4.0f)
               && nearlyEqual(swr, 1.5f)
               && model.hasRecentReflectedPower(500));

    model.removeMeter(9);
    emitted = false;
    reflectedPowerMeasured = true;
    model.updateValues({8, 10}, {rawDb(50.0f), rawDb(1.5f)});

    report("missing REFPWR requests calculated fallback",
           emitted && !reflectedPowerMeasured
               && nearlyEqual(model.reflectedPower(), 0.0f)
               && !model.hasRecentReflectedPower(500));

    model.clear();
    report("disconnect clears reflected-power state",
           nearlyEqual(model.reflectedPower(), 0.0f)
               && model.reflectedPowerUpdatedAtMs() == 0);
}

void testNativeSwrRemainsRadioProvidedAtLowPower()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    float emittedSwr = 0.0f;
    QObject::connect(&model, &MeterModel::txMetersChanged,
                     [&emittedSwr](float, float swr) {
        emittedSwr = swr;
    });

    model.updateValues({8, 10}, {rawDb(6.1f), rawDb(1.0859375f)});

    report("MeterModel preserves the radio-native SWR sample at low power",
           nearlyEqual(model.fwdPowerInstant(), 0.004f)
               && nearlyEqual(model.swr(), 1.0859375f)
               && nearlyEqual(emittedSwr, 1.0859375f));
}

// #4540: the fast-attack/slow-decay smoothing on FWDPWR is right during a
// transmission and wrong at the end of one. A radio with no carrier reports
// 0 dBm = 0.001 W, and an exponential decay converges on that rather than
// reaching it, so the display kept claiming forward power after unkey.
void testForwardPowerSnapsToZeroWhenTheCarrierStops()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));

    // Transmitting: ~36.5 dBm is about 4.5 W, the level measured on the bench.
    model.updateValues({8}, {rawDb(36.5f)});
    const bool keyed = model.fwdPower() > 1.0f;

    // Unkey. The radio reports 0 dBm — NOT a small power, no power.
    model.updateValues({8}, {rawDb(0.0f)});

    report("MeterModel drops forward power to zero on the first no-carrier sample",
           keyed && nearlyEqual(model.fwdPower(), 0.0f));
}

// The decay is what made this visible on the bench: without the fix the reading
// was still 3.45 W 200 ms after unkey and took ~2.9 s to fall away. Feeding
// several no-carrier samples reproduces exactly that window.
void testForwardPowerDoesNotLingerAcrossRepeatedNoCarrierSamples()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));

    model.updateValues({8}, {rawDb(36.5f)});
    for (int i = 0; i < 5; ++i) {
        model.updateValues({8}, {rawDb(0.0f)});
    }

    report("MeterModel reports no forward power while the carrier is absent",
           nearlyEqual(model.fwdPower(), 0.0f));
}

// The smoothing must survive for real readings — this is a display filter that
// exists for a reason (#980), and the fix must not flatten it.
void testForwardPowerStillSmoothsRealReadings()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));

    model.updateValues({8}, {rawDb(36.5f)});          // ~4.5 W, first sample
    const float first = model.fwdPower();
    model.updateValues({8}, {rawDb(30.0f)});          // ~1.0 W, a real drop

    // Slow-decay smoothing means the displayed value must LAG the new reading,
    // sitting between the two rather than snapping to the lower one.
    const float after = model.fwdPower();
    report("MeterModel still smooths a genuine drop in forward power",
           first > 4.0f && after < first && after > 1.5f);
}

// The threshold has to catch the no-carrier floor WITHOUT swallowing a genuine
// low-power reading, so pin the boundary from the other side: a value just above
// kNoCarrierWatts must stay on the smoothed path rather than snapping to zero.
//
// 0.7 dBm is ~0.00117 W — a hair above the 0.0011 W threshold, and the closest a
// real reading can plausibly sit to it. If a future change widens the threshold
// this is the test that fails.
void testForwardPowerJustAboveTheThresholdStaysSmoothed()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));

    model.updateValues({8}, {rawDb(36.5f)});          // ~4.5 W, establish a level
    const float keyed = model.fwdPower();
    model.updateValues({8}, {rawDb(0.7f)});           // ~0.00117 W, above the floor

    // Smoothed, so it must LAG rather than snap: still well above zero after one
    // sample. A snap-to-zero here would mean the threshold had eaten a real
    // reading.
    const float after = model.fwdPower();
    report("MeterModel keeps smoothing a reading just above the no-carrier floor",
           keyed > 4.0f && after > 0.01f);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testAdjacentMetersDoNotSynthesizeCompression();
    testCompPeakDirectlyExposesCompression();
    testCompPeakClampsToGaugeRange();
    testActiveTxSliceSelectsCompPeak();
    testZeroSourceCompPeakUsesSliceContext();
    testSparseSliceIdsUseManifestDerivedWaveformBase();
    testAfterEqAndScMicDoNotAffectCompression();
    testRemovingCompPeakMarksCompressionUnavailable();
    testRemovingAdjacentMetersDoesNotClearCompPeak();
    testDirectionalPowerUsesDirectReflectedMeter();
    testNativeSwrRemainsRadioProvidedAtLowPower();
    testForwardPowerSnapsToZeroWhenTheCarrierStops();
    testForwardPowerDoesNotLingerAcrossRepeatedNoCarrierSamples();
    testForwardPowerStillSmoothsRealReadings();
    testForwardPowerJustAboveTheThresholdStaysSmoothed();

    return g_failed == 0 ? 0 : 1;
}
