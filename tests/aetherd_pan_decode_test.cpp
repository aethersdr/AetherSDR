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

    // ---- Facet 3: model sinks ----
    {
        PanadapterModel pan(QStringLiteral("0x40000000"));
        QSignalSpy info(&pan, &PanadapterModel::infoChanged);

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
