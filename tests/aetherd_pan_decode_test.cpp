// Socket-free production display decoder and normalized model contracts.
#include "FlexDisplayStatusTestSupport.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (false)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const auto epoch = std::make_shared<std::atomic_bool>(true);
    const auto decode = [&](const QMap<QString, QString>& values, bool waterfall = false) {
        return decodeFlexDisplayStatus(waterfall ? QStringLiteral("display waterfall 0xF2000001")
                                                : QStringLiteral("display pan 0xC0000001"), values, epoch);
    };
    const auto all = decode({{"center", "7.15"}, {"bandwidth", "0.2"}, {"min_dbm", "-125"},
        {"max_dbm", "-40"}, {"rfgain", "-8"}, {"rxant", "RX_A"}, {"ant_list", "ANT1,ANT2"},
        {"fps", "25"}, {"average", "0"}, {"weighted_average", "255"}, {"wide", "1"},
        {"loopa", "0"}, {"loopb", "1"}, {"pre", "20"}, {"daxiq_channel", "2"},
        {"band_zoom", "1"}, {"segment_zoom", "0"}, {"noise_floor_range", "3.5"},
        {"wnb", "1"}, {"wnb_level", "42"}, {"wnb_updating", "1"},
        {"x_pixels", "1024"}, {"y_pixels", "700"}, {"client_handle", "0xABCDEF01"},
        {"waterfall", "0xF2000001"}});
    CHECK(all && all->id == QLatin1String("0xc0000001") && all->ownerHandle == 0xabcdef01U
          && all->pairedId == QStringLiteral("0xf2000001"));
    CHECK(all->values.centerMhz == 7.15 && all->values.bandwidthMhz == 0.2
          && all->values.minDbm == -125 && all->values.maxDbm == -40);
    CHECK(all->values.rfGain == -8 && all->values.rxAntenna == QStringLiteral("RX_A")
          && all->values.antennas == QStringList({QStringLiteral("ANT1"), QStringLiteral("ANT2")}));
    CHECK(all->values.fps == 25 && all->values.average == 0 && all->values.weightedAverage == true);
    CHECK(all->values.wide == true && all->values.loopA == false && all->values.loopB == true);
    CHECK(all->values.bandZoom == true && all->values.segmentZoom == false
          && all->values.noiseFloorRange == 3.5 && all->values.daxIqChannel == 2);
    CHECK(all->values.wnb == true && all->values.wnbLevel == 42 && all->values.wnbUpdating == true);
    CHECK(all->values.xPixels == 1024 && all->values.yPixels == 700 && all->values.preamp == QStringLiteral("20"));
    const auto partial = decode({{"center", "7.1"}, {"unrelated", "ignored"}});
    CHECK(partial && partial->values.centerMhz == 7.1 && !partial->values.bandwidthMhz
          && !partial->values.fps && !partial->ownerHandle && !partial->pairedId);
    for (const QString& bad : {QStringLiteral("garbage"), QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("1e300")}) {
        const auto invalid = decode({{"center", bad}, {"bandwidth", bad}, {"min_dbm", bad},
            {"max_dbm", "-40"}, {"noise_floor_range", bad}, {"rfgain", bad}, {"fps", bad}, {"average", bad}});
        CHECK(invalid && !invalid->values.centerMhz && !invalid->values.bandwidthMhz && !invalid->values.minDbm
              && invalid->values.maxDbm == -40 && !invalid->values.noiseFloorRange && !invalid->values.rfGain
              && !invalid->values.fps && !invalid->values.average);
    }
    for (const QString& bad : {QStringLiteral("garbage"), QStringLiteral("-1"), QStringLiteral("2")}) {
        const auto invalid = decode({{"wide", bad}, {"loopa", bad}, {"loopb", bad}, {"band_zoom", bad},
                                     {"segment_zoom", bad}, {"wnb", bad}, {"wnb_updating", bad}});
        CHECK(invalid && !invalid->values.wide && !invalid->values.loopA && !invalid->values.loopB
            && !invalid->values.bandZoom && !invalid->values.segmentZoom && !invalid->values.wnb && !invalid->values.wnbUpdating);
    }
    CHECK(!decode({{"weighted_average", "256"}, {"wnb_level", "101"}, {"daxiq_channel", "-1"}})->values.weightedAverage);
    CHECK(!decode({{"wnb_level", "101"}})->values.wnbLevel);
    CHECK(!decode({{"wnb_level", "-1"}})->values.wnbLevel);
    CHECK(decode({{"wnb_level", "100"}})->values.wnbLevel == 100);
    CHECK(!decode({{"daxiq_channel", "4294967295"}})->values.daxIqChannel);
    CHECK(!decode({{"client_handle", "bogus"}}) && !decode({{"waterfall", "0x100000000"}}));
    CHECK(decode({{"client_handle", "0"}})->ownerHandle == 0);
    CHECK(decode({{"waterfall", "0"}})->pairedId == QString());
    CHECK(decode({{"waterfall", "42000000"}})->pairedId == QStringLiteral("0x0280de80"));
    CHECK(decode({{"client_handle", "4294967295"}})->ownerHandle == 0xffffffffU);
    CHECK(!decode({{"client_handle", "4294967296"}}));
    CHECK(!decodeFlexDisplayStatus("display pan 0x40000000junk", {}, epoch));
    CHECK(!decodeFlexDisplayStatus("display pan 0x100000000", {}, epoch));
    CHECK(!decodeFlexDisplayStatus("display pan 0x0", {}, epoch));
    CHECK(!decodeFlexDisplayStatus("display pan 0x40000000 unrelated", {}, epoch));
    CHECK(decodeFlexDisplayStatus("display pan 0x40000000 removed", {}, epoch)->removed);
    CHECK(decode({{"removed", "1"}})->removed && !decode({{"removed", "0"}}));
    const auto waterfall = decode({{"center", "14.2"}, {"bandwidth", "0.1"}, {"line_duration", "65"},
                                   {"panadapter", "0xC0000001"}}, true);
    CHECK(waterfall && waterfall->pairedId == QStringLiteral("0xc0000001")
          && waterfall->values.centerMhz == 14.2 && waterfall->values.waterfallRate == 65);
    CHECK(!decode({{"line_duration", "bad"}}, true)->values.waterfallRate);
    QMap<QString, QString> oversized;
    for (int i = 0; i < 129; ++i) { oversized[QString::number(i)] = "v"; }
    CHECK(!decode(oversized));
    CHECK(!decode({{"unused", QString(16385, 'a')}}));
    {
        PanadapterModel pan(QStringLiteral("0xc0000001"));
        QSignalSpy fps(&pan, &PanadapterModel::fpsReported);
        QSignalSpy waterfallRate(&pan, &PanadapterModel::waterfallLineDurationReported);
        pan.applyDisplayValues(all->values, {});
        CHECK(pan.fps() == 25 && pan.average() == 0 && pan.wnbLevel() == 42 && pan.weightedAverageKnown());
        const quint64 revision = pan.fftObservationRevision();
        pan.applyDisplayValues(all->values, {});
        CHECK(fps.count() == 2 && pan.fftObservationRevision() == revision + 2);
        pan.applyDisplayValues(waterfall->values, {});
        pan.applyDisplayValues(waterfall->values, {});
        CHECK(waterfallRate.count() == 2);
        pan.setOwnerHandle(0);
        CHECK(!pan.ownedByClient(0x71));
        pan.setOwnerHandle(0x71);
        CHECK(pan.ownedByClient(0x71) && !pan.ownedByClient(0x72));
        applyPanReport(pan, {{"band_zoom", "1"}, {"segment_zoom", "0"}});
        applyPanReport(pan, {{"band_zoom", "bad"}, {"segment_zoom", "2"}});
        CHECK(pan.bandZoomOn() && !pan.segmentZoomOn());
    }
    epoch->store(false);
    CHECK(!all->current());
    // ---- Facet 3: model sinks ----
    {
        PanadapterModel pan(QStringLiteral("0x40000000"));
        QSignalSpy info(&pan, &PanadapterModel::infoChanged);

        // The numeric center default is only a placeholder until a normalized
        // update arrives. An update equal to that default still marks it known
        // and emits one edge so consumers can replace their fallback; repeated
        // identical updates remain quiet (#3913 review).
        CHECK(pan.centerKnown() == false);
        pan.setCenterBandwidth(14.1, -1.0);
        CHECK(pan.centerKnown() == true);
        CHECK(info.count() == 1);
        info.clear();
        pan.setCenterBandwidth(14.1, -1.0);
        CHECK(info.count() == 0);

        // Negative = "leave unchanged": bandwidth held, only center moves.
        const double origBw = pan.bandwidthMhz();
        pan.setCenterBandwidth(7.15, -1.0);
        CHECK(qFuzzyCompare(pan.centerMhz(), 7.15));
        CHECK(qFuzzyCompare(pan.bandwidthMhz(), origBw));
        CHECK(info.count() == 1);
        info.clear();

        // No actual change → no emission.
        pan.setCenterBandwidth(7.15, -1.0);
        CHECK(info.count() == 0);

        // Reconnect staging may retain the model object, but its old numeric
        // center must not remain authoritative for TCI dds: in the new session.
        pan.resetCenterKnownForReconnect();
        CHECK(pan.centerKnown() == false);
        CHECK(qFuzzyCompare(pan.centerMhz(), 7.15));
        CHECK(info.count() == 0);
        pan.setCenterBandwidth(7.15, -1.0);
        CHECK(pan.centerKnown() == true);
        CHECK(info.count() == 1);
        info.clear();

        // setRange: NaN = "leave unchanged" (max held, only min moves); returns
        // whether anything changed (gates the setDbmRange side-effect).
        QSignalSpy lvl(&pan, &PanadapterModel::levelChanged);
        const float origMax = pan.maxDbm();
        CHECK(pan.setRange(-125.0, std::nan("")) == true);
        CHECK(qFuzzyCompare(pan.minDbm(), -125.0f));
        CHECK(qFuzzyCompare(pan.maxDbm(), origMax));
        CHECK(lvl.count() == 1);
        lvl.clear();
        // Same values → no change, no emission, returns false.
        CHECK(pan.setRange(-125.0, std::nan("")) == false);
        CHECK(lvl.count() == 0);

        QSignalSpy wnbSpy(&pan, &PanadapterModel::wnbStateChanged);
        applyPanReport(pan, {{"wnb", "1"}, {"wnb_level", "100"}});
        applyPanReport(pan, {{"wnb_level", "250"}});
        CHECK(pan.wnbActive() && pan.wnbLevel() == 100 && wnbSpy.count() == 1);

    }

    // ---- Facet 3c: model sink — setBandwidthLimits (the zoom clamp's source) ----
    //
    // The X-axis counterpart to setRange. Unlike dBm bounds, span limits are only
    // meaningful as a PAIR: the consumer clamps the operator's zoom between them,
    // so a half-applied or inverted report would pin the span to a single value
    // with no way back out. Every rejection below leaves the previous state — and
    // "unknown" — intact, which is what keeps a Flex radio on its model-derived
    // clamp rather than on a bad report.
    {
        PanadapterModel pan(QStringLiteral("0x40000001"));
        QSignalSpy limits(&pan, &PanadapterModel::bandwidthLimitsChanged);

        // Nothing reported yet: the GUI must be able to tell this apart from a
        // real limit, or it would clamp every radio to zero.
        CHECK(pan.bandwidthLimitsKnown() == false);

        CHECK(pan.setBandwidthLimits(0.048, 0.384) == true);
        CHECK(pan.bandwidthLimitsKnown() == true);
        CHECK(qFuzzyCompare(pan.minBandwidthMhz(), 0.048));
        CHECK(qFuzzyCompare(pan.maxBandwidthMhz(), 0.384));
        CHECK(limits.count() == 1);
        limits.clear();

        // Idempotent: same pair → no change, no emission.
        CHECK(pan.setBandwidthLimits(0.048, 0.384) == false);
        CHECK(limits.count() == 0);

        // Inverted (min above max) is REJECTED wholesale, not clamped or
        // reordered: a backend that reports it is confused, and guessing which
        // bound it meant would hand the GUI a limit nobody asked for.
        CHECK(pan.setBandwidthLimits(0.384, 0.048) == false);
        CHECK(qFuzzyCompare(pan.minBandwidthMhz(), 0.048));
        CHECK(qFuzzyCompare(pan.maxBandwidthMhz(), 0.384));
        CHECK(limits.count() == 0);

        // Non-positive bounds are not limits. Zero is the "never reported"
        // sentinel, so accepting it would make a bad report indistinguishable
        // from silence.
        CHECK(pan.setBandwidthLimits(0.0, 0.384) == false);
        CHECK(pan.setBandwidthLimits(0.048, 0.0) == false);
        CHECK(pan.setBandwidthLimits(-1.0, 0.384) == false);
        CHECK(limits.count() == 0);
        CHECK(pan.bandwidthLimitsKnown() == true);   // still the good pair

        // A single rate (min == max) is a legitimate radio: one span, no zoom.
        CHECK(pan.setBandwidthLimits(0.192, 0.192) == true);
        CHECK(limits.count() == 1);
    }

    if (failures == 0) {
        std::printf("aetherd_pan_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_pan_decode_test: %d failure(s)\n", failures);
    return 1;
}
