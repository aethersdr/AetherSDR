// aetherd RFC 2.3 — MeterModel touchpoint: FlexBackend::decodeMeterStatus.
// Pins the SmartSDR meter-status wire decode (definitions + removal) that moved
// out of RadioModel::handleMeterStatus. (#4070: typed MeterDef payload.)

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/MeterDef.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static MeterDef decode1(FlexBackend& b, const QString& body)
{
    QSignalSpy def(&b, &IRadioBackend::meterDefined);
    b.decodeMeterStatus(body);
    if (def.count() != 1) return {};
    return def.takeFirst().at(0).value<MeterDef>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<MeterDef>();

    // ---- full definition ----
    {
        FlexBackend backend;
        const MeterDef d = decode1(backend, QStringLiteral(
            "7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0"));
        CHECK(d.index == 7);
        CHECK(d.source == QStringLiteral("SLC"));
        CHECK(d.sourceIndex == 0);
        CHECK(d.name == QStringLiteral("LEVEL"));
        CHECK(d.unit == QStringLiteral("dBm"));
        CHECK(qFuzzyCompare(d.low, -150.0));
        CHECK(qFuzzyCompare(d.high, 20.0));
        CHECK(d.description.isEmpty());   // absent key → MeterDef default
    }

    // ---- removal ----
    {
        FlexBackend backend;
        QSignalSpy rem(&backend, &IRadioBackend::meterRemoved);
        backend.decodeMeterStatus(QStringLiteral("7 removed"));
        CHECK(rem.count() == 1);
        CHECK(rem.takeFirst().at(0).toInt() == 7);
    }

    // ---- multiple meters in one body, grouped by index (ascending) ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral(
            "1.src=TX#1.nam=FWDPWR#1.unit=Watts#2.src=TX#2.nam=SWR#2.unit=SWR"));
        CHECK(def.count() == 2);
        const MeterDef a = def.takeFirst().at(0).value<MeterDef>();
        CHECK(a.index == 1 && a.name == QStringLiteral("FWDPWR"));
        const MeterDef b = def.takeFirst().at(0).value<MeterDef>();
        CHECK(b.index == 2 && b.name == QStringLiteral("SWR"));
    }

    // ---- malformed index token skipped, no emission ----
    {
        FlexBackend backend;
        QSignalSpy def(&backend, &IRadioBackend::meterDefined);
        backend.decodeMeterStatus(QStringLiteral("x.src=SLC#x.nam=LEVEL"));
        CHECK(def.count() == 0);
    }

    // ---- malformed numerics leave the MeterDef default; valid fields carried.
    //      NOTE: for a plain MeterDef field the default (0/0.0) equals what an
    //      unguarded parse of a malformed value yields, so this does NOT pin the
    //      carry() ok-guard — that contract is pinned where nullopt ≠ 0, at the
    //      std::optional carry() sites (aetherd_slice_decode_test /
    //      aetherd_transmit_decode_test malformed cases). (#4075 review.)
    {
        FlexBackend backend;
        const MeterDef d = decode1(backend, QStringLiteral(
            "3.nam=LEVEL#3.low=junk#3.hi=20.0#3.num=nope"));
        CHECK(d.name == QStringLiteral("LEVEL"));
        CHECK(qFuzzyCompare(d.high, 20.0));   // valid field carried
        CHECK(d.low == 0.0);                  // malformed → MeterDef default 0.0
        CHECK(d.sourceIndex == 0);            // malformed → MeterDef default 0
    }

    if (g_failures == 0) {
        std::printf("aetherd_meter_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_meter_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
