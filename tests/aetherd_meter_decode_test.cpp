// aetherd RFC 2.3 — MeterModel touchpoint: FlexBackend::decodeMeterStatus.
// Pins the SmartSDR meter-status wire decode (definitions + removal) that moved
// out of RadioModel::handleMeterStatus.

#include "core/backends/flex/FlexBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QVariantMap>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- full definition ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral(
            "7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0"));
        CHECK(def.count() == 1);
        {
            const QList<QVariant> a = def.takeFirst();
            CHECK(a.at(0).toInt() == 7);
            const QVariantMap f = a.at(1).toMap();
            CHECK(f.value(QStringLiteral("source")).toString() == QStringLiteral("SLC"));
            CHECK(f.value(QStringLiteral("sourceIndex")).toInt() == 0);
            CHECK(f.value(QStringLiteral("name")).toString() == QStringLiteral("LEVEL"));
            CHECK(f.value(QStringLiteral("unit")).toString() == QStringLiteral("dBm"));
            CHECK(qFuzzyCompare(f.value(QStringLiteral("low")).toDouble(), -150.0));
            CHECK(qFuzzyCompare(f.value(QStringLiteral("high")).toDouble(), 20.0));
            CHECK(!f.contains(QStringLiteral("description")));  // absent key not carried
        }
    }

    // ---- removal ----
    {
        FlexBackend backend;
        QSignalSpy rem(&backend, &IRadioBackend::meterRemoved);
        backend.decodeMeterStatus(QStringLiteral("7 removed"));
        CHECK(rem.count() == 1);
        CHECK(rem.takeFirst().at(0).toInt() == 7);
    }

    // ---- multiple meters in one body, grouped by index ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral(
            "1.src=TX#1.nam=FWDPWR#1.unit=Watts#2.src=TX#2.nam=SWR#2.unit=SWR"));
        CHECK(def.count() == 2);
        // QMap groups by ascending index → meter 1 first.
        const QList<QVariant> a = def.takeFirst();
        CHECK(a.at(0).toInt() == 1);
        CHECK(a.at(1).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("FWDPWR"));
        const QList<QVariant> b = def.takeFirst();
        CHECK(b.at(0).toInt() == 2);
        CHECK(b.at(1).toMap().value(QStringLiteral("name")).toString() == QStringLiteral("SWR"));
    }

    // ---- malformed index token skipped, no emission ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral("x.src=SLC#x.nam=LEVEL"));
        CHECK(def.count() == 0);
    }

    // ---- malformed numeric fields dropped, not applied as 0 (#4066 guard) ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral(
            "3.nam=LEVEL#3.low=junk#3.hi=20.0#3.num=nope"));
        CHECK(def.count() == 1);
        const QVariantMap f = def.takeFirst().at(1).toMap();
        CHECK(f.value(QStringLiteral("name")).toString() == QStringLiteral("LEVEL"));
        CHECK(!f.contains(QStringLiteral("low")));         // malformed → dropped
        CHECK(!f.contains(QStringLiteral("sourceIndex"))); // malformed num → dropped
        CHECK(f.contains(QStringLiteral("high")));         // valid field still carried
    }

    if (g_failures == 0) {
        std::printf("aetherd_meter_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_meter_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
