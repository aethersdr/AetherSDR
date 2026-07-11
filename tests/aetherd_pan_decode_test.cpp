// aetherd RFC 2.3 template coverage: the pan decode/normalize chain.
//
// Exercises the three facets the 2.3 template introduces on the pan touchpoint:
//   1. DECODE    — FlexBackend::decodePanCenterBandwidth → panCenterBandwidthChanged
//   2. EXTENSION — FlexBackend::decodePanExtensions      → extensionStatus("flex","panWnb")
//   3. the model sinks — PanadapterModel::setCenterBandwidth / applyWnbExtension
//
// Notably pins the wnb_level numeric guard (#4063 review): a malformed
// wnb_level must be dropped, not applied as 0.

#include "core/backends/flex/FlexBackend.h"
#include "models/PanadapterModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVariantMap>
#include <QString>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- Facet 1: DECODE (center/bandwidth) ----
    {
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::panCenterBandwidthChanged);

        // Neither field present → no emission (matches old touch-only behavior).
        backend.decodePanCenterBandwidth(QStringLiteral("0x40000000"),
                                         {{QStringLiteral("min_dbm"), QStringLiteral("-120")}});
        CHECK(spy.count() == 0);

        // center only → emitted, bandwidth carries the -1 "unchanged" sentinel.
        backend.decodePanCenterBandwidth(QStringLiteral("0x40000000"),
                                         {{QStringLiteral("center"), QStringLiteral("7.15")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(a.at(0).toString() == QStringLiteral("0x40000000"));
            CHECK(qFuzzyCompare(a.at(1).toDouble(), 7.15));
            CHECK(a.at(2).toDouble() < 0.0);
        }

        // both present → both carried through.
        backend.decodePanCenterBandwidth(QStringLiteral("0x40000000"),
                                         {{QStringLiteral("center"), QStringLiteral("14.2")},
                                          {QStringLiteral("bandwidth"), QStringLiteral("0.2")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(qFuzzyCompare(a.at(1).toDouble(), 14.2));
            CHECK(qFuzzyCompare(a.at(2).toDouble(), 0.2));
        }
    }

    // ---- Facet 1b: DECODE (min/max dBm) + the NaN "unchanged" sentinel ----
    {
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::panRangeChanged);

        // Neither field present → no emission.
        backend.decodePanRange(QStringLiteral("0x40000000"),
                               {{QStringLiteral("center"), QStringLiteral("7.1")}});
        CHECK(spy.count() == 0);

        // min_dbm only → emitted; max carries NaN (unchanged), min is signed.
        backend.decodePanRange(QStringLiteral("0x40000000"),
                               {{QStringLiteral("min_dbm"), QStringLiteral("-130")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(qFuzzyCompare(a.at(1).toDouble(), -130.0));
            CHECK(std::isnan(a.at(2).toDouble()));
        }

        // both present → both carried (both negative — proves no numeric sentinel).
        backend.decodePanRange(QStringLiteral("0x40000000"),
                               {{QStringLiteral("min_dbm"), QStringLiteral("-125")},
                                {QStringLiteral("max_dbm"), QStringLiteral("-40")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(qFuzzyCompare(a.at(1).toDouble(), -125.0));
            CHECK(qFuzzyCompare(a.at(2).toDouble(), -40.0));
        }

        // Malformed present min_dbm → carried as NaN (unchanged), NOT 0.0 dBm
        // (which would collapse the scale). max still parses. (#4065 review)
        backend.decodePanRange(QStringLiteral("0x40000000"),
                               {{QStringLiteral("min_dbm"), QStringLiteral("junk")},
                                {QStringLiteral("max_dbm"), QStringLiteral("-40")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(std::isnan(a.at(1).toDouble()));
            CHECK(qFuzzyCompare(a.at(2).toDouble(), -40.0));
        }
    }

    // ---- Facet 2: EXTENSION (WNB) + the wnb_level guard ----
    {
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::extensionStatus);

        // No WNB keys → no extension emission.
        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("center"), QStringLiteral("7.1")}});
        CHECK(spy.count() == 0);

        // Valid wnb + wnb_level → namespaced under "flex"/"panWnb", both present.
        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("wnb"), QStringLiteral("1")},
                                     {QStringLiteral("wnb_level"), QStringLiteral("42")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(a.at(0).toString() == QStringLiteral("flex"));
            CHECK(a.at(1).toString() == QStringLiteral("panWnb"));
            const QVariantMap f = a.at(2).toMap();
            CHECK(f.value(QStringLiteral("panId")).toString() == QStringLiteral("0x40000000"));
            CHECK(f.value(QStringLiteral("wnb")).toBool() == true);
            CHECK(f.contains(QStringLiteral("wnb_level")));
            CHECK(f.value(QStringLiteral("wnb_level")).toInt() == 42);
        }

        // Malformed wnb_level → the key must be DROPPED, not applied as 0
        // (#4063 review — Principle VII). wnb still carries.
        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("wnb"), QStringLiteral("1")},
                                     {QStringLiteral("wnb_level"), QStringLiteral("garbage")}});
        CHECK(spy.count() == 1);
        {
            const QVariantMap f = spy.takeFirst().at(2).toMap();
            CHECK(f.value(QStringLiteral("wnb")).toBool() == true);
            CHECK(!f.contains(QStringLiteral("wnb_level")));   // guarded out
        }

        // #4147 audit: wnb / wnb_updating mirror FlexLib's uint.TryParse +
        // > 1 reject (Panadapter.cs 1226/1262) — malformed AND out-of-range
        // values are dropped from the carry, not coerced to a bool. wnb_level
        // additionally rejects > 100 and negatives (Panadapter.cs 1244) instead
        // of leaving them to a model-side clamp.
        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("wnb"), QStringLiteral("bogus")},
                                     {QStringLiteral("wnb_updating"), QStringLiteral("5")},
                                     {QStringLiteral("wnb_level"), QStringLiteral("150")}});
        CHECK(spy.count() == 0);   // every key rejected → nothing to carry

        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("wnb"), QStringLiteral("2")},
                                     {QStringLiteral("wnb_level"), QStringLiteral("-5")},
                                     {QStringLiteral("wnb_updating"), QStringLiteral("1")}});
        CHECK(spy.count() == 1);   // wnb_updating alone survives
        {
            const QVariantMap f = spy.takeFirst().at(2).toMap();
            CHECK(!f.contains(QStringLiteral("wnb")));         // 2 > 1 → dropped
            CHECK(!f.contains(QStringLiteral("wnb_level")));   // negative → dropped
            CHECK(f.value(QStringLiteral("wnb_updating")).toBool() == true);
        }

        // Boundary values still pass: wnb_level=100 is the FlexLib max.
        backend.decodePanExtensions(QStringLiteral("0x40000000"),
                                    {{QStringLiteral("wnb"), QStringLiteral("0")},
                                     {QStringLiteral("wnb_level"), QStringLiteral("100")}});
        CHECK(spy.count() == 1);
        {
            const QVariantMap f = spy.takeFirst().at(2).toMap();
            CHECK(f.value(QStringLiteral("wnb")).toBool() == false);
            CHECK(f.value(QStringLiteral("wnb_level")).toInt() == 100);
        }
    }

    // ---- Facet 1c: DECODE (rfgain / antenna — universal) ----
    {
        FlexBackend backend;
        QSignalSpy gain(&backend, &IRadioBackend::panRfGainChanged);
        QSignalSpy rxant(&backend, &IRadioBackend::panRxAntennaChanged);
        QSignalSpy antlist(&backend, &IRadioBackend::panAntennaListChanged);

        backend.decodePanRfGain(QStringLiteral("0x40000000"),
                                {{QStringLiteral("rfgain"), QStringLiteral("-8")}});
        CHECK(gain.count() == 1);
        CHECK(gain.takeFirst().at(1).toInt() == -8);   // signed gain preserved

        // Malformed rfgain → dropped, not emitted as 0. (#4065 review)
        backend.decodePanRfGain(QStringLiteral("0x40000000"),
                                {{QStringLiteral("rfgain"), QStringLiteral("nope")}});
        CHECK(gain.count() == 0);

        backend.decodePanAntenna(QStringLiteral("0x40000000"),
                                 {{QStringLiteral("rxant"), QStringLiteral("ANT2")},
                                  {QStringLiteral("ant_list"), QStringLiteral("ANT1,ANT2,RX_A")}});
        CHECK(rxant.count() == 1);
        CHECK(rxant.takeFirst().at(1).toString() == QStringLiteral("ANT2"));
        CHECK(antlist.count() == 1);
        CHECK(antlist.takeFirst().at(1).toStringList().size() == 3);
    }

    // ---- Facet 2b: EXTENSION (panState bundle) + client_handle #3977 ----
    {
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::extensionStatus);

        // No panState keys → no emission.
        backend.decodePanState(QStringLiteral("0x40000000"),
                               {{QStringLiteral("center"), QStringLiteral("7.1")}});
        CHECK(spy.count() == 0);

        backend.decodePanState(QStringLiteral("0x40000000"),
                               {{QStringLiteral("wide"), QStringLiteral("1")},
                                {QStringLiteral("loopa"), QStringLiteral("1")},
                                {QStringLiteral("daxiq_channel"), QStringLiteral("3")},
                                {QStringLiteral("client_handle"), QStringLiteral("0x5C0FFEE0")},
                                {QStringLiteral("waterfall"), QStringLiteral("0x42000000")}});
        CHECK(spy.count() == 1);
        {
            const QList<QVariant> a = spy.takeFirst();
            CHECK(a.at(0).toString() == QStringLiteral("flex"));
            CHECK(a.at(1).toString() == QStringLiteral("panState"));
            const QVariantMap f = a.at(2).toMap();
            CHECK(f.value(QStringLiteral("panId")).toString() == QStringLiteral("0x40000000"));
            CHECK(f.contains(QStringLiteral("wide")));
            CHECK(f.contains(QStringLiteral("client_handle")));
            CHECK(!f.contains(QStringLiteral("fps")));   // absent key not carried
        }
    }

    // ---- Facet 1d: DECODE (waterfall line_duration guard) ----
    {
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::panWaterfallLineDurationChanged);
        backend.decodeWaterfallLineDuration(QStringLiteral("0x40000000"),
                                            {{QStringLiteral("line_duration"), QStringLiteral("100")}});
        CHECK(spy.count() == 1);
        CHECK(spy.takeFirst().at(1).toInt() == 100);
        // Malformed → dropped, not applied as 0.
        backend.decodeWaterfallLineDuration(QStringLiteral("0x40000000"),
                                            {{QStringLiteral("line_duration"), QStringLiteral("nope")}});
        CHECK(spy.count() == 0);
    }

    // ---- Facet 3b: model sink — applyStateExtension incl. #3977 ownership ----
    {
        PanadapterModel pan(QStringLiteral("0x40000000"));
        QSignalSpy wide(&pan, &PanadapterModel::wideChanged);
        QSignalSpy dax(&pan, &PanadapterModel::daxiqChannelChanged);
        QVariantMap st;
        st.insert(QStringLiteral("wide"), QStringLiteral("1"));
        st.insert(QStringLiteral("daxiq_channel"), QStringLiteral("3"));
        st.insert(QStringLiteral("client_handle"), QStringLiteral("0x5C0FFEE0"));
        pan.applyStateExtension(st);
        CHECK(pan.wideActive() == true);
        CHECK(wide.count() == 1);
        CHECK(pan.daxiqChannel() == 3);
        CHECK(dax.count() == 1);
        // #3977: owner handle tracked (parsed hex, non-zero).
        CHECK(pan.ownerHandle() == 0x5C0FFEE0u);
        // A zero client_handle must NOT overwrite a known owner (fail-closed).
        QVariantMap z; z.insert(QStringLiteral("client_handle"), QStringLiteral("0x0"));
        pan.applyStateExtension(z);
        CHECK(pan.ownerHandle() == 0x5C0FFEE0u);
    }

    // ---- Facet 2c: band/segment zoom — carry + model semantics (#4057) ----
    {
        // Backend: the two radio-owned zoom flags ride the panState bundle.
        FlexBackend backend;
        QSignalSpy spy(&backend, &IRadioBackend::extensionStatus);
        backend.decodePanState(QStringLiteral("0x40000000"),
                               {{QStringLiteral("band_zoom"), QStringLiteral("1")},
                                {QStringLiteral("segment_zoom"), QStringLiteral("0")}});
        CHECK(spy.count() == 1);
        {
            const QVariantMap f = spy.takeFirst().at(2).toMap();
            CHECK(f.value(QStringLiteral("band_zoom")).toString() == QStringLiteral("1"));
            CHECK(f.value(QStringLiteral("segment_zoom")).toString() == QStringLiteral("0"));
        }

        // Model: FlexLib parse semantics verbatim (Panadapter.cs 933/1159) —
        // 0/1 apply with change-gated signals; >1 or non-numeric are invalid
        // and skipped, not applied as 0 (Principle VII).
        PanadapterModel pan(QStringLiteral("0x40000000"));
        QSignalSpy band(&pan, &PanadapterModel::bandZoomChanged);
        QSignalSpy seg(&pan, &PanadapterModel::segmentZoomChanged);
        CHECK(pan.bandZoomOn() == false);      // default off
        CHECK(pan.segmentZoomOn() == false);

        QVariantMap on;
        on.insert(QStringLiteral("band_zoom"), QStringLiteral("1"));
        pan.applyStateExtension(on);
        CHECK(pan.bandZoomOn() == true);
        CHECK(band.count() == 1);
        CHECK(band.takeFirst().at(0).toBool() == true);

        // Same value again → no re-emit (change-gated, like FlexLib's
        // continue-on-equal).
        pan.applyStateExtension(on);
        CHECK(band.count() == 0);

        // The radio clearing band_zoom while engaging segment_zoom lands as
        // one bundle; both flags apply independently (radio owns exclusion).
        QVariantMap swap;
        swap.insert(QStringLiteral("band_zoom"), QStringLiteral("0"));
        swap.insert(QStringLiteral("segment_zoom"), QStringLiteral("1"));
        pan.applyStateExtension(swap);
        CHECK(pan.bandZoomOn() == false);
        CHECK(pan.segmentZoomOn() == true);
        CHECK(band.count() == 1);
        CHECK(seg.count() == 1);
        band.clear(); seg.clear();

        // Invalid values (>1, non-numeric, negative) → skipped, state held.
        QVariantMap bad;
        bad.insert(QStringLiteral("band_zoom"), QStringLiteral("2"));
        bad.insert(QStringLiteral("segment_zoom"), QStringLiteral("nope"));
        pan.applyStateExtension(bad);
        QVariantMap neg;
        neg.insert(QStringLiteral("segment_zoom"), QStringLiteral("-1"));
        pan.applyStateExtension(neg);
        CHECK(pan.bandZoomOn() == false);
        CHECK(pan.segmentZoomOn() == true);    // still on — invalids ignored
        CHECK(band.count() == 0);
        CHECK(seg.count() == 0);
    }

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

        // applyWnbExtension applies only present keys and clamps the level.
        QSignalSpy wnbSpy(&pan, &PanadapterModel::wnbStateChanged);
        QVariantMap ext;
        ext.insert(QStringLiteral("wnb"), true);
        ext.insert(QStringLiteral("wnb_level"), 250);   // out of range → clamp to 100
        pan.applyWnbExtension(ext);
        CHECK(pan.wnbActive() == true);
        CHECK(pan.wnbLevel() == 100);
        CHECK(wnbSpy.count() == 1);
    }

    if (g_failures == 0) {
        std::printf("aetherd_pan_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_pan_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
