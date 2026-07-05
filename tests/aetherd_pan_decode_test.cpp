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
