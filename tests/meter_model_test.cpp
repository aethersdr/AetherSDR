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

    return g_failed == 0 ? 0 : 1;
}
