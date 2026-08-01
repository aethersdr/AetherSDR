#include "models/MeterModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
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

    return g_failed == 0 ? 0 : 1;
}
