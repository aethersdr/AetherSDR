#include "models/MeterModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
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

// ONE modulator, however many receivers: the gauge must follow transmit.
//
// The HL2 shape — a single SLC def at sourceIndex 0 and a single implicit-source
// COMPPEAK, which defineMeter() therefore files under implicit slice 0. Move
// transmit to the second receiver (the VFO widget, the RX applet and the
// cycle-TX shortcut all do) and m_activeTxSlice becomes 1, the by-slice lookup
// misses, and the compression gauge went dead for a compressor that was still
// working (#4609 review). Delete the single-implicit-entry fallback in
// compPeakIndexForActiveTxSlice() and this fails.
void testSingleImplicitCompPeakFollowsTransmitToAnySlice()
{
    MeterModel model;
    model.defineMeter(slcMeter(1, 0));
    model.defineMeter(txMeter(8, "COMPPEAK", "dB", 0));

    model.setActiveTxSlice(0);
    model.updateValues({8}, {rawDb(6.0f)});
    report("a single implicit COMPPEAK resolves on the manifest's own slice",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 6.0f));

    model.setActiveTxSlice(1);
    model.updateValues({8}, {rawDb(9.0f)});
    report("a single implicit COMPPEAK follows transmit onto another slice",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 9.0f));
}

// ...and the fallback stays narrow. A radio that declares COMPPEAK per
// TX-waveform slice is answering "which transmitter" for itself, so an implicit
// meter alongside it must not be volunteered for a slice it was not filed under
// — that would point the gauge at the wrong transmitter, which is worse than a
// gauge that reads zero.
void testImplicitCompPeakIsNotVolunteeredWhenAnExplicitMapExists()
{
    MeterModel model;
    model.defineMeter(slcMeter(15, 0));
    model.defineMeter(txMeter(23, "COMPPEAK", "dB", 8));   // explicit, slice 0
    model.defineMeter(slcMeter(37, 1));

    model.setActiveTxSlice(1);
    model.updateValues({23}, {rawDb(12.0f)});
    report("an explicit per-waveform COMPPEAK map is never second-guessed",
           !model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 0.0f));
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

// THE UNIT CONTRACT. This model used to interpret a meter purely by NAME and
// apply a unit it assumed, so a backend publishing its radio's honest unit was
// silently mis-rendered. Both cases below reproduce a live IC-705 defect.
// hasMicPeakMeter() is what MainWindow's mic-gauge visibility follows, and it
// flips LATE — the meter list arrives after the connect edge. RadioModel
// re-publishes capabilities on that transition, so the predicate has to be
// honest in both directions or the gauge sticks.
void testMicPeakAvailabilityTracksTheMeterList()
{
    MeterModel model;
    report("no mic-peak meter before the radio declares one", !model.hasMicPeakMeter());

    model.defineMeter(slcMeter(10, 0));
    report("an unrelated meter does not make one appear", !model.hasMicPeakMeter());

    model.defineMeter(txMeter(31, "MICPEAK", "dBFS"));
    report("MICPEAK makes it available", model.hasMicPeakMeter());

    model.removeMeter(31);
    report("and removing it makes it unavailable again", !model.hasMicPeakMeter());
}

void testForwardPowerHonoursItsDeclaredUnit()
{
    // Watts, declared. 5 W must stay 5 W.
    {
        MeterModel model;
        model.defineMeter(txMeter(8, "FWDPWR", "Watts"));
        // NOT rawDb(): only the dB-family units are scaled by 128 on the way
        // in, so a Watts meter's raw value IS its value.
        model.updateValues({8}, {5});
        report("a FWDPWR meter declared in Watts is NOT converted from dBm",
               nearlyEqual(model.fwdPowerInstant(), 5.0f));
        // The old behaviour, pinned so the fix cannot silently regress: read as
        // dBm, 5 becomes 10^(5/10)/1000 = 0.00316 W and the gauge never moves.
        report("and the pre-fix dBm reading really was ~0.003 W",
               std::pow(10.0f, 5.0f / 10.0f) / 1000.0f < 0.01f);
    }
    // dBm stays the default for every backend that predates the field.
    {
        MeterModel model;
        model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
        model.updateValues({8}, {rawDb(50.0f)});
        report("a FWDPWR meter declared in dBm still converts to watts",
               nearlyEqual(model.fwdPowerInstant(), 100.0f));
    }
    {
        MeterModel model;
        model.defineMeter(txMeter(8, "FWDPWR", QString{}));
        model.updateValues({8}, {50});
        report("an undeclared unit is treated as dBm, as it always was",
               nearlyEqual(model.fwdPowerInstant(), 100.0f));
    }
}

// REFPWR was missed when FWDPWR and ALC were fixed, and MeterSurfaces.h already
// advertised this consumer as accepting Watts — so the join reported a
// watts-declaring backend as unit-agreeing while the value was still converted
// from dBm. The diagnostic vouching for the bug is worse than the bug.
void testReflectedPowerHonoursItsDeclaredUnit()
{
    {
        MeterModel model;
        model.defineMeter(txMeter(8, "FWDPWR", "Watts"));
        model.defineMeter(txMeter(9, "REFPWR", "Watts"));
        model.updateValues({8, 9}, {10, 1});
        report("a REFPWR meter declared in Watts is NOT converted from dBm",
               nearlyEqual(model.reflectedPower(), 1.0f));
    }
    {
        MeterModel model;
        model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
        model.defineMeter(txMeter(9, "REFPWR", "dBm"));
        model.updateValues({8, 9}, {rawDb(50.0f), rawDb(36.0206f)});
        report("and a dBm one still converts, as every existing backend expects",
               nearlyEqual(model.reflectedPower(), 4.0f));
    }
}

void testAlcPercentIsMappedOntoTheGaugeRange()
{
    // The ALC consumers are a -20..0 dBFS gauge. A radio running its own ALC
    // reports a percentage of ITS full scale; handed over raw it pins the gauge
    // and stays there, which is what "ALC is completely pegged" looked like.
    MeterModel model;
    model.defineMeter(txMeter(11, "ALC", "Percent"));
    model.setActiveTxSlice(0);

    float alc = 999.0f;
    QObject::connect(&model, &MeterModel::swAlcChanged, [&alc](float v) { alc = v; });

    model.updateValues({11}, {0});
    report("0 % ALC lands at the gauge floor", nearlyEqual(alc, -20.0f));

    model.updateValues({11}, {50});
    report("50 % ALC lands mid-scale", nearlyEqual(alc, -10.0f));

    model.updateValues({11}, {100});
    report("100 % ALC lands at the gauge ceiling", nearlyEqual(alc, 0.0f));

    // A dBFS backend must be untouched — this is a mapping for radios that
    // cannot speak dBFS, not a reinterpretation of the ones that can.
    MeterModel dbfs;
    dbfs.defineMeter(txMeter(11, "ALC", "dBFS"));
    dbfs.setActiveTxSlice(0);
    float passthrough = 999.0f;
    QObject::connect(&dbfs, &MeterModel::swAlcChanged,
                     [&passthrough](float v) { passthrough = v; });
    dbfs.updateValues({11}, {rawDb(-6.0f)});
    report("a dBFS ALC meter passes through unchanged", nearlyEqual(passthrough, -6.0f));
}

void testActiveTxSliceSelectsAlcAndItsUnit()
{
    MeterModel model;
    model.defineMeter(slcMeter(15, 0));
    model.defineMeter(txMeter(23, "ALC", "dBFS", 8));
    model.defineMeter(slcMeter(37, 1));
    model.defineMeter(txMeter(45, "ALC", "Percent", 9));

    model.setActiveTxSlice(0);
    model.updateValues({23}, {rawDb(-6.0f)});
    report("active TX slice 0 uses its ALC meter and dBFS unit",
           nearlyEqual(model.swAlc(), -6.0f));

    model.updateValues({45}, {50});
    report("inactive ALC meter is ignored", nearlyEqual(model.swAlc(), -6.0f));

    model.setActiveTxSlice(1);
    report("changing active TX slice clears stale ALC", nearlyEqual(model.swAlc(), -20.0f));

    model.updateValues({45}, {50});
    report("active TX slice 1 uses its ALC meter and Percent unit",
           nearlyEqual(model.swAlc(), -10.0f));
}

void testMixedSourceAlcUsesManifestSliceContext()
{
    // FLEX-8400M fw 4.2.18 declares slice A's TX waveform block with num=0,
    // then slice B's with num=9. Source-index arithmetic cannot map that pair;
    // the preceding SLC block is the radio's stable association.
    MeterModel model;
    model.defineMeter(slcMeter(12, 0));
    model.defineMeter(txMeter(22, "ALC", "dBFS", 0));
    model.defineMeter(slcMeter(30, 1));
    model.defineMeter(txMeter(40, "ALC", "dBFS", 9));

    model.setActiveTxSlice(1);
    model.updateValues({22}, {rawDb(-3.0f)});
    report("8400M slice B ignores slice A's zero-source ALC",
           nearlyEqual(model.swAlc(), -20.0f));

    model.updateValues({40}, {rawDb(-6.4f)});
    report("8400M slice B resolves ALC from manifest context",
           nearlyEqual(model.swAlc(), -6.4f));
}

void testMixedSourceTxWaveformMetersUseManifestSliceContext()
{
    MeterModel model;
    model.defineMeter(slcMeter(12, 0));
    model.defineMeter(txMeter(18, "SC_MIC", "dBFS", 0));
    model.defineMeter(txMeter(20, "COMPPEAK", "dB", 0));
    model.defineMeter(txMeter(21, "SC_FILT_1", "dBFS", 0));
    model.defineMeter(txMeter(24, "SC_FILT_2", "dBFS", 0));
    model.defineMeter(slcMeter(30, 1));
    model.defineMeter(txMeter(36, "SC_MIC", "dBFS", 9));
    model.defineMeter(txMeter(38, "COMPPEAK", "dB", 9));
    model.defineMeter(txMeter(39, "SC_FILT_1", "dBFS", 9));
    model.defineMeter(txMeter(42, "SC_FILT_2", "dBFS", 9));

    model.setActiveTxSlice(1);
    model.updateValues({36, 38, 39, 42},
                       {rawDb(-12.0f), rawDb(8.0f), rawDb(-9.0f), rawDb(-15.0f)});

    report("8400M slice B resolves COMPPEAK from manifest context",
           model.hasCompressionMeterValue() && nearlyEqual(model.compPeak(), 8.0f));
    report("8400M slice B resolves TX filter levels from manifest context",
           model.hasTxFilterLevels() && nearlyEqual(model.scFilt1(), -9.0f)
               && nearlyEqual(model.scFilt2(), -15.0f));
}

void testZeroSourceAlcUsesSliceContext()
{
    MeterModel model;
    model.defineMeter(slcMeter(14, 0));
    model.defineMeter(txMeter(20, "ALC", "dBFS", 0));
    model.defineMeter(slcMeter(32, 1));
    model.defineMeter(txMeter(44, "ALC", "dBFS", 0));

    model.setActiveTxSlice(1);
    model.updateValues({20}, {rawDb(-4.0f)});
    report("inactive zero-source ALC meter is ignored", nearlyEqual(model.swAlc(), -20.0f));

    model.updateValues({44}, {rawDb(-12.0f)});
    report("zero-source ALC meter follows active slice context",
           nearlyEqual(model.swAlc(), -12.0f));
}

void testSingleImplicitAlcFollowsTransmitToAnySlice()
{
    MeterModel model;
    model.defineMeter(slcMeter(1, 0));
    model.defineMeter(txMeter(8, "ALC", "dBFS", 0));

    model.setActiveTxSlice(1);
    model.updateValues({8}, {rawDb(-9.0f)});
    report("a single implicit ALC follows transmit onto another slice",
           nearlyEqual(model.swAlc(), -9.0f));

    model.removeMeter(8);
    report("removing the active ALC meter clears its value",
           nearlyEqual(model.swAlc(), -20.0f));
}

void testTxMeterRedefinitionsPreserveTheirSlice()
{
    for (bool implicit : {false, true}) {
        MeterModel model;
        const QStringList names{"ALC", "COMPPEAK", "SC_MIC", "SC_FILT_1", "SC_FILT_2"};
        for (int slice = 0; slice < 2; ++slice) {
            model.defineMeter(slcMeter(10 + 20 * slice, slice));
            for (int i = 0; i < names.size(); ++i) {
                model.defineMeter(txMeter(20 + 20 * slice + i, names[i],
                                          names[i] == "COMPPEAK" ? "dB" : "dBFS",
                                          implicit ? 0 : 8 + slice));
            }
        }
        model.setActiveTxSlice(1);
        // A profile re-announces A's definitions after B's SLC context. The
        // same meter identity must retain its original ownership and units
        // must still update. No synthetic radio or socket is involved.
        for (int i = 0; i < names.size(); ++i) {
            model.defineMeter(txMeter(20 + i, names[i],
                                      names[i] == "ALC" ? "Percent"
                                          : names[i] == "COMPPEAK" ? "dB" : "dBFS",
                                      implicit ? 0 : 8));
        }
        model.updateValues({20, 21, 22, 23, 24, 40, 41, 42, 43, 44},
                           {50, rawDb(3), rawDb(-3), rawDb(-4), rawDb(-5),
                            rawDb(-8), rawDb(12), rawDb(-10), rawDb(-11), rawDb(-12)});
        report("A's redefinition cannot replace B's ALC/compression/filter ownership",
               nearlyEqual(model.swAlc(), -8) && nearlyEqual(model.compPeak(), 12)
                   && nearlyEqual(model.scMic(), -10) && nearlyEqual(model.scFilt1(), -11)
                   && nearlyEqual(model.scFilt2(), -12));
        model.setActiveTxSlice(0);
        model.updateValues({20}, {50});
        report("a redefined ALC unit updates without changing its slice",
               nearlyEqual(model.swAlc(), -10));
        model.removeMeter(40);
        model.setActiveTxSlice(1);
        model.updateValues({20}, {50});
        report("a redefinition leaves no duplicate slice alias for an ALC meter",
               implicit ? nearlyEqual(model.swAlc(), -10)
                        : nearlyEqual(model.swAlc(), -20));
    }
}

void testAlcClearsToPresentationFloor()
{
    for (const QString& unit : {QStringLiteral("dBFS"), QStringLiteral("Percent")}) {
        MeterModel model;
        report("ALC starts at the presentation floor", nearlyEqual(model.swAlc(), -20));
        model.defineMeter(slcMeter(10, 0));
        model.defineMeter(txMeter(20, "ALC", unit, 8));
        model.defineMeter(slcMeter(30, 1));
        model.defineMeter(txMeter(40, "ALC", unit, 9));
        model.setActiveTxSlice(1);
        model.updateValues({40}, {unit == "Percent" ? qint16(50) : rawDb(-8)});
        float emitted = 999;
        int emissions = 0;
        QObject::connect(&model, &MeterModel::swAlcChanged, [&](float value) {
            emitted = value;
            ++emissions;
        });
        model.setActiveTxSlice(0);
        report("a TX slice change emits the empty ALC presentation value",
               emissions == 1 && nearlyEqual(emitted, -20));
        model.setActiveTxSlice(0);
        report("re-selecting the TX slice does not emit another clear", emissions == 1);
        model.updateValues({20}, {unit == "Percent" ? qint16(50) : rawDb(-8)});
        model.removeMeter(20);
        report("active ALC removal emits the empty presentation value",
               emissions == 3 && nearlyEqual(emitted, -20));
        model.clear();
        report("disconnect resets ALC to the presentation floor", nearlyEqual(model.swAlc(), -20));
    }
}

void testTxMeterIdentityReuseAndContextLifetime()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(20, "ALC", "dBFS", 8));
    model.defineMeter(slcMeter(30, 1));
    model.defineMeter(txMeter(40, "ALC", "dBFS", 9));
    model.setActiveTxSlice(1);
    model.updateValues({40}, {rawDb(-8)});
    MeterDef replacement = txMeter(40, "UNRELATED", "dBFS", 9);
    model.defineMeter(replacement);
    model.updateValues({40}, {rawDb(-3)});
    report("an index reused for another meter cannot keep its ALC route",
           nearlyEqual(model.swAlc(), -20));
    model.removeMeter(30);
    model.removeMeter(10);
    model.removeMeter(20);
    model.removeMeter(40);
    model.defineMeter(txMeter(60, "ALC", "dBFS", 8));
    model.defineMeter(txMeter(80, "ALC", "dBFS", 9));
    model.setActiveTxSlice(0);
    model.updateValues({60, 80}, {rawDb(-6), rawDb(-12)});
    report("removed SLC context cannot poison a later context-free explicit map",
           nearlyEqual(model.swAlc(), -6));
    model.setActiveTxSlice(1);
    model.updateValues({60, 80}, {rawDb(-6), rawDb(-12)});
    report("context-free explicit ALC resolves the second slice", nearlyEqual(model.swAlc(), -12));

    MeterModel repurposed;
    repurposed.defineMeter(slcMeter(10, 0));
    repurposed.defineMeter(txMeter(20, "ALC", "dBFS", 0));
    repurposed.defineMeter(txMeter(40, "SC_MIC", "dBFS", 0));
    repurposed.defineMeter(slcMeter(30, 1));
    repurposed.defineMeter(txMeter(40, "ALC", "dBFS", 9));
    repurposed.setActiveTxSlice(1);
    repurposed.updateValues({20, 40}, {rawDb(-6), rawDb(-12)});
    report("repurposing an ID retains the incoming definition's SLC context",
           nearlyEqual(repurposed.swAlc(), -12));
}

void testExplicitAlcIsNotVolunteeredToAnotherSlice()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(20, "ALC", "dBFS", 8));
    model.defineMeter(slcMeter(30, 1));
    model.setActiveTxSlice(1);
    model.updateValues({20}, {rawDb(-3)});
    report("a lone explicitly associated ALC is not assigned to another slice",
           nearlyEqual(model.swAlc(), -20));
}

void testImplicitModulatorBeforeTxSelectionAndUnrelatedContext()
{
    MeterModel implicit;
    implicit.defineMeter(txMeter(20, "ALC", "dBFS", 0));
    implicit.defineMeter(txMeter(21, "COMPPEAK", "dB", 0));
    implicit.updateValues({20, 21}, {rawDb(-8), rawDb(6)});
    report("one implicit modulator can publish before a TX slice is selected",
           nearlyEqual(implicit.swAlc(), -8) && nearlyEqual(implicit.compPeak(), 6));
    MeterModel separate;
    separate.defineMeter(slcMeter(10, 1));
    MeterDef unrelated;
    unrelated.index = 15;
    unrelated.source = "RAD";
    unrelated.name = "PATEMP";
    unrelated.unit = "degC";
    separate.defineMeter(unrelated);
    separate.defineMeter(txMeter(20, "ALC", "dBFS", 8));
    separate.defineMeter(txMeter(40, "ALC", "dBFS", 9));
    separate.setActiveTxSlice(1);
    // The legacy fallback's base is 8 - min(SLC=1); slice 1 resolves source 8.
    separate.updateValues({20, 40}, {rawDb(-6), rawDb(-12)});
    report("an unrelated manifest block ends SLC context before explicit fallback",
           nearlyEqual(separate.swAlc(), -6));
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
                                               float ratio, bool /*swrValid*/,
                                               bool measured) {
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

// Find a meter entry by name in the allMeters() array.
QJsonObject meterNamed(const QJsonArray& all, const QString& name)
{
    for (const auto& v : all) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("name")).toString() == name)
            return o;
    }
    return {};
}

// #4533: SWR is derived from forward/reflected power, so once the TX meters go
// stale it is a leftover from the previous transmit, not a measurement. Showing
// it as live sends the operator hunting a non-existent antenna fault.
void testSwrIsLiveWhileTxMetersAreFresh()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    model.updateValues({8, 10}, {rawDb(30.0f), rawDb(2.88f)});

    const QJsonObject swr = meterNamed(model.allMeters(), QStringLiteral("SWR"));
    report("MeterModel reports SWR as live immediately after a TX sample",
           swr.value(QStringLiteral("has_value")).toBool()
               && nearlyEqual(static_cast<float>(swr.value(QStringLiteral("value")).toDouble()),
                              2.88f)
               && model.swrIfLive().has_value());
}

// The GUI reads SWR from the SIGNALS, not from allMeters(). Gating only the
// array left the displayed value untouched — an HL2 showed 255.99 on screen
// while the meter list correctly reported no value. Assert the signals too.
void testStaleSwrIsNotEmittedToConsumers()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    float txSwr = -1.0f;
    float dirSwr = -1.0f;
    bool txSwrValid = false;
    bool dirSwrValid = false;
    QObject::connect(&model, &MeterModel::txMetersChanged,
                     [&txSwr, &txSwrValid](float, float swr, bool valid) {
        txSwr = swr; txSwrValid = valid;
    });
    QObject::connect(&model, &MeterModel::directionalPowerMetersChanged,
                     [&dirSwr, &dirSwrValid](float, float, float swr,
                                             bool valid, bool) {
        dirSwr = swr; dirSwrValid = valid;
    });

    // A real transmit — forward power AND a ratio in the same packet. Both
    // signals must carry the measured value.
    model.updateValues({8, 10}, {rawDb(36.5f), rawDb(2.5f)});
    const bool liveOk = nearlyEqual(txSwr, 2.5f) && nearlyEqual(dirSwr, 2.5f);

    // Now the case that reached the screen: the carrier stops, so forward power
    // reads 0 dBm, and the radio keeps chattering a saturated SWR — 255.99, the
    // ratio of two noise samples — with no power behind it.
    model.updateValues({8, 10}, {rawDb(0.0f), rawDb(255.99f)});

    report("MeterModel does not emit a saturated SWR with no forward power (txMetersChanged)",
           liveOk && txSwr < 100.0f && !txSwrValid);
    report("MeterModel does not emit a saturated SWR with no forward power (directional)",
           liveOk && dirSwr < 100.0f && !dirSwrValid);
}

// The configuration the review found uncovered (#4536 blocker 1): a backend
// that declares FWDPWR but NEVER publishes it — the HL2, whose forward counts
// are uncalibrated so it deliberately sends only TX:SWR, itself gated at the
// source on real drive. Its SWR must publish as VALID on its own freshness:
// the earlier forward-power gate zeroed it forever, hiding a real mismatch on
// the one backend whose SWR is most trustworthy.
void testSwrWithoutForwardPowerBackendIsValid()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));   // declared, never sent
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    float txSwr = -1.0f;
    bool  txSwrValid = false;
    QObject::connect(&model, &MeterModel::txMetersChanged,
                     [&txSwr, &txSwrValid](float, float swr, bool valid) {
        txSwr = swr; txSwrValid = valid;
    });

    // HL2 keyed into a genuine 3:1 mismatch: SWR arrives, FWDPWR never does.
    model.updateValues({10}, {rawDb(3.0f)});

    report("HL2-shaped backend (SWR, no FWDPWR ever) publishes a VALID SWR",
           txSwrValid && nearlyEqual(txSwr, 3.0f)
               && model.swrIfLive().has_value());

    // And the same reading goes ABSENT once the sample itself ages out.
    model.setLastSwrUpdateMsForTest(
        QDateTime::currentMSecsSinceEpoch() - MeterModel::kTxMeterStaleMs - 1);
    report("HL2-shaped backend SWR goes absent when the sample ages",
           !model.swrIfLive().has_value());
}

void testSwrIsSuppressedOnceTxMetersGoStale()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    model.updateValues({8, 10}, {rawDb(30.0f), rawDb(2.88f)});

    // Age the TX meters past the staleness window without sleeping.
    model.setLastTxMeterUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    // SWR gates on its own stamp now (#4536) — age it in step.
    model.setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));

    const QJsonObject swr = meterNamed(model.allMeters(), QStringLiteral("SWR"));
    report("MeterModel suppresses a stale SWR reading (has_value=false, age=-1)",
           swr.value(QStringLiteral("has_value")).toBool() == false
               && swr.value(QStringLiteral("value")).isNull()
               && swr.value(QStringLiteral("age_ms")).toInt() == -1
               && !model.swrIfLive().has_value());
}

// metersForSource() is a second view of the same model, and it duplicated
// allMeters()' loop WITHOUT the gate — so a `tx`-source query answered
// has_value=true for the very meter `all` reported as absent. Two views of one
// model disagreeing about one reading is the disagreement #4533 was filed about,
// so both have to answer the same way.
void testStaleSwrIsAlsoSuppressedInMetersForSource()
{
    MeterModel model;
    model.defineMeter(txMeter(8, "FWDPWR", "dBm"));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    model.updateValues({8, 10}, {rawDb(30.0f), rawDb(2.88f)});
    model.setLastTxMeterUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    // SWR gates on its own stamp now (#4536) — age it in step.
    model.setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));

    const QJsonObject fromAll =
        meterNamed(model.allMeters(), QStringLiteral("SWR"));
    const QJsonArray txSource = model.metersForSource(QStringLiteral("TX-"), 8);
    const QJsonObject fromSource =
        meterNamed(txSource, QStringLiteral("SWR"));

    report("MeterModel suppresses a stale SWR in metersForSource too",
           // Guard against a vacuous pass: the query must actually have matched.
           !txSource.isEmpty()
               && fromSource.value(QStringLiteral("has_value")).toBool() == false
               && fromSource.value(QStringLiteral("value")).isNull()
               && fromSource.value(QStringLiteral("age_ms")).toInt() == -1
               // and the two views agree, which is the actual invariant
               && fromSource.value(QStringLiteral("has_value"))
                      == fromAll.value(QStringLiteral("has_value")));
}

// The suppression must be specific to SWR — a receive meter that has nothing to
// do with the transmit path must keep reporting while the radio is idle.
void testStaleTxMetersDoNotSuppressReceiveMeters()
{
    MeterModel model;
    model.defineMeter(slcMeter(3, 0));
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    model.updateValues({3, 10}, {rawDb(-64.0f), rawDb(2.88f)});
    model.setLastTxMeterUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    // SWR gates on its own stamp now (#4536) — age it in step.
    model.setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));

    const QJsonArray all = model.allMeters();
    const QJsonObject swr = meterNamed(all, QStringLiteral("SWR"));
    const QJsonObject lvl = meterNamed(all, QStringLiteral("LEVEL"));

    report("MeterModel keeps receive meters live when the TX meters are stale",
           swr.value(QStringLiteral("has_value")).toBool() == false
               && lvl.value(QStringLiteral("has_value")).toBool() == true);
}

// The freshness flag the support-bundle snapshot publishes must agree with the
// SWR gate it is meant to explain.
//
// RadioModel::troubleshootingSnapshot() reports tx_forward_power_w (which holds
// its last smoothed value) next to tx_swr (which goes null once the TX meters
// go stale), and qualifies the pair with tx_meters_fresh. If that flag could
// ever read true while swrIfLive() returned nullopt, the caveat would be absent
// exactly when it is needed and the reader would take the held wattage for a
// live one. Both derive from hasRecentTxMeters(kTxMeterStaleMs); this pins that
// they cannot disagree. (#4533 review)
// Suppression must not LATCH. After a stale window, the next SWR sample has to
// restore liveness — that is the property an operator depends on the first time
// they key up following a long receive period, and it is the one direction the
// other tests never exercise: they start fresh and go stale, never the reverse.
//
// Without this, a regression that suppressed SWR permanently once it had aged
// out would still pass every other case here. (#4536 review)
void testSwrRecoversAfterAFreshSampleFollowsAStaleWindow()
{
    MeterModel model;
    model.defineMeter(txMeter(10, "SWR", "SWR"));

    model.updateValues({10}, {rawDb(2.88f)});
    const bool liveBefore = model.swrIfLive().has_value();

    // Age it past the window, the way a long receive period would.
    model.setLastTxMeterUpdateMsForTest(
        QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    model.setLastSwrUpdateMsForTest(
        QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    const bool suppressedWhileStale = !model.swrIfLive().has_value();
    const QJsonObject staleEntry = meterNamed(model.allMeters(), QStringLiteral("SWR"));
    const bool arrayAbsentWhileStale =
        !staleEntry.value(QStringLiteral("has_value")).toBool();

    // Key up again: one fresh sample, nothing else changed.
    model.updateValues({10}, {rawDb(1.42f)});

    const auto recovered = model.swrIfLive();
    const QJsonObject liveEntry = meterNamed(model.allMeters(), QStringLiteral("SWR"));

    report("MeterModel restores SWR liveness when a fresh sample follows a stale window",
           liveBefore && suppressedWhileStale && arrayAbsentWhileStale
               && recovered.has_value() && nearlyEqual(*recovered, 1.42f)
               && liveEntry.value(QStringLiteral("has_value")).toBool()
               && nearlyEqual(
                      static_cast<float>(liveEntry.value(QStringLiteral("value")).toDouble()),
                      1.42f));
}

void testSwrLivenessAgreesWithTxMeterFreshness()
{
    MeterModel model;
    model.defineMeter(txMeter(10, "SWR", "SWR"));
    model.updateValues({10}, {rawDb(2.88f)});

    const bool freshWhileLive = model.hasRecentTxMeters(MeterModel::kTxMeterStaleMs);
    const bool haveSwrWhileLive = model.swrIfLive().has_value();

    model.setLastTxMeterUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));
    // SWR gates on its own stamp now (#4536) — age it in step.
    model.setLastSwrUpdateMsForTest(QDateTime::currentMSecsSinceEpoch() - (MeterModel::kTxMeterStaleMs + 500));

    const bool freshWhenStale = model.hasRecentTxMeters(MeterModel::kTxMeterStaleMs);
    const bool haveSwrWhenStale = model.swrIfLive().has_value();

    report("MeterModel TX-meter freshness and SWR liveness agree in both directions",
           freshWhileLive && haveSwrWhileLive
               && !freshWhenStale && !haveSwrWhenStale);
}

} // namespace


// --- TX-filter taps (#4649) -------------------------------------------------

// Regression guard for the bug that would have shipped: the check used to also
// require SC_MIC, which is the PC/remote-audio entry point ONLY. A hardware mic
// (BAL/LINE/ACC) joins the chain later via the CODEC ADC and never registers
// there, so requiring it silently disabled the whole feature for every operator
// on a real microphone. The filter taps alone must be sufficient.
void testTxFilterLevelsDoNotRequireScMic()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(29, "SC_FILT_1", "dBFS"));
    model.defineMeter(txMeter(32, "SC_FILT_2", "dBFS"));
    model.setActiveTxSlice(0);

    // No SC_MIC meter defined at all, and none ever published.
    model.updateValues({29, 32}, {rawDb(-8.0f), rawDb(-70.0f)});

    report("TX filter levels are usable without SC_MIC (hardware-mic operators)",
           model.hasTxFilterLevels()
               && nearlyEqual(model.scFilt1(), -8.0f)
               && nearlyEqual(model.scFilt2(), -70.0f));
}

// SC_FILT_1 publishes at 20 fps and SC_FILT_2 at 10, so emitting on whichever
// arrives would hand a consumer one fresh value and a partner up to 100 ms old.
// Publication is tied to the slower tap; a lone SC_FILT_1 update must stay quiet.
void testTxFilterLevelsPublishOnTheSlowerTap()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(29, "SC_FILT_1", "dBFS"));
    model.defineMeter(txMeter(32, "SC_FILT_2", "dBFS"));
    model.setActiveTxSlice(0);

    int emissions = 0;
    QObject::connect(&model, &MeterModel::txFilterLevelsChanged,
                     &model, [&emissions](float, float) { ++emissions; });

    model.updateValues({29}, {rawDb(-8.0f)});          // fast tap alone
    const bool quietOnFastTap = (emissions == 0);

    model.updateValues({32}, {rawDb(-70.0f)});         // slow tap closes the pair
    const bool emitsOnSlowTap = (emissions == 1);

    report("TX filter levels publish on the slower tap only",
           quietOnFastTap && emitsOnSlowTap);
}

// A radio can publish one TX waveform meter block per active slice. Keying a
// single index per meter would be last-definition-wins, and the TX-filter check
// would silently watch some other slice's filter.
void testActiveTxSliceSelectsTxFilterTaps()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(slcMeter(11, 1));
    model.defineMeter(txMeter(29, "SC_FILT_1", "dBFS", 8));
    model.defineMeter(txMeter(32, "SC_FILT_2", "dBFS", 8));
    model.defineMeter(txMeter(59, "SC_FILT_1", "dBFS", 9));
    model.defineMeter(txMeter(62, "SC_FILT_2", "dBFS", 9));

    model.setActiveTxSlice(1);
    model.updateValues({29, 32, 59, 62},
                       {rawDb(-1.0f), rawDb(-2.0f), rawDb(-8.0f), rawDb(-70.0f)});

    report("active TX slice selects its own TX filter taps",
           model.hasTxFilterLevels()
               && nearlyEqual(model.scFilt1(), -8.0f)
               && nearlyEqual(model.scFilt2(), -70.0f));
}

// The stored levels describe one slice's chain. After a slice change a
// comparison must not straddle the two.
void testChangingActiveTxSliceDropsStaleFilterLevels()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(29, "SC_FILT_1", "dBFS", 8));
    model.defineMeter(txMeter(32, "SC_FILT_2", "dBFS", 8));
    // Declare the taps in slice A's block before moving to slice B's block.
    model.defineMeter(slcMeter(11, 1));
    model.setActiveTxSlice(0);
    model.updateValues({29, 32}, {rawDb(-8.0f), rawDb(-70.0f)});
    const bool hadLevels = model.hasTxFilterLevels();

    model.setActiveTxSlice(1);

    report("changing the active TX slice drops stale filter levels",
           hadLevels && !model.hasTxFilterLevels());
}

// Removing either tap must invalidate the pair rather than leave a level
// describing a meter that no longer exists.
void testRemovingATxFilterTapInvalidatesThePair()
{
    MeterModel model;
    model.defineMeter(slcMeter(10, 0));
    model.defineMeter(txMeter(29, "SC_FILT_1", "dBFS"));
    model.defineMeter(txMeter(32, "SC_FILT_2", "dBFS"));
    model.setActiveTxSlice(0);
    model.updateValues({29, 32}, {rawDb(-8.0f), rawDb(-70.0f)});
    const bool hadLevels = model.hasTxFilterLevels();

    model.removeMeter(32);

    report("removing a TX filter tap invalidates the pair",
           hadLevels && !model.hasTxFilterLevels());
}

void testPaCurrentIsDistinctFromTemperature()
{
    MeterModel model;
    MeterDef current;
    current.index = 90;
    current.source = "RAD";
    current.name = "PACURRENT";
    current.unit = "Amps";
    current.low = 0.0;
    current.high = 25.0;
    model.defineMeter(current);

    float published = -1.0f;
    QObject::connect(&model, &MeterModel::paCurrentChanged,
                     [&published](float amps) { published = amps; });
    model.updateValueByName("RAD", "PACURRENT", 7.5f);

    report("PA current is exposed without fabricating PA temperature",
           model.hasPaCurrentMeter() && model.hasPaCurrent()
               && nearlyEqual(model.paCurrent(), 7.5f)
               && nearlyEqual(published, 7.5f) && !model.hasPaTemp());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testAdjacentMetersDoNotSynthesizeCompression();
    testCompPeakDirectlyExposesCompression();
    testCompPeakClampsToGaugeRange();
    testActiveTxSliceSelectsCompPeak();
    testZeroSourceCompPeakUsesSliceContext();
    testSingleImplicitCompPeakFollowsTransmitToAnySlice();
    testImplicitCompPeakIsNotVolunteeredWhenAnExplicitMapExists();
    testSparseSliceIdsUseManifestDerivedWaveformBase();
    testAfterEqAndScMicDoNotAffectCompression();
    testRemovingCompPeakMarksCompressionUnavailable();
    testRemovingAdjacentMetersDoesNotClearCompPeak();
    testMicPeakAvailabilityTracksTheMeterList();
    testForwardPowerHonoursItsDeclaredUnit();
    testReflectedPowerHonoursItsDeclaredUnit();
    testAlcPercentIsMappedOntoTheGaugeRange();
    testActiveTxSliceSelectsAlcAndItsUnit();
    testMixedSourceAlcUsesManifestSliceContext();
    testMixedSourceTxWaveformMetersUseManifestSliceContext();
    testZeroSourceAlcUsesSliceContext();
    testSingleImplicitAlcFollowsTransmitToAnySlice();
    testTxMeterRedefinitionsPreserveTheirSlice();
    testAlcClearsToPresentationFloor();
    testTxMeterIdentityReuseAndContextLifetime();
    testExplicitAlcIsNotVolunteeredToAnotherSlice();
    testImplicitModulatorBeforeTxSelectionAndUnrelatedContext();
    testDirectionalPowerUsesDirectReflectedMeter();
    testNativeSwrRemainsRadioProvidedAtLowPower();
    testForwardPowerSnapsToZeroWhenTheCarrierStops();
    testForwardPowerDoesNotLingerAcrossRepeatedNoCarrierSamples();
    testForwardPowerStillSmoothsRealReadings();
    testForwardPowerJustAboveTheThresholdStaysSmoothed();
    testSwrIsLiveWhileTxMetersAreFresh();
    testSwrIsSuppressedOnceTxMetersGoStale();
    testStaleSwrIsNotEmittedToConsumers();
    testSwrWithoutForwardPowerBackendIsValid();
    testStaleSwrIsAlsoSuppressedInMetersForSource();
    testStaleTxMetersDoNotSuppressReceiveMeters();
    testSwrRecoversAfterAFreshSampleFollowsAStaleWindow();
    testSwrLivenessAgreesWithTxMeterFreshness();

    testTxFilterLevelsDoNotRequireScMic();
    testTxFilterLevelsPublishOnTheSlowerTap();
    testActiveTxSliceSelectsTxFilterTaps();
    testChangingActiveTxSliceDropsStaleFilterLevels();
    testRemovingATxFilterTapInvalidatesThePair();
    testPaCurrentIsDistinctFromTemperature();

    return g_failed == 0 ? 0 : 1;
}
