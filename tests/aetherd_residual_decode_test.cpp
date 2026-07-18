// aetherd RFC 2.3 (RadioModel residual) — FlexBackend GPS / memory / profile
// decode. Pins the wire → typed delta translation that moved out of
// RadioModel::handleGpsStatus / handleMemoryStatus / handleProfileStatus(Raw):
// '#'-tokenised GPS, memory-slot kv-set + removal, and the "profile <type> …"
// list/current + importing/exporting flags.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/GpsDelta.h"
#include "core/backends/MemoryDelta.h"
#include "core/backends/ProfileDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static GpsDelta decodeGps(FlexBackend& b, const QString& body)
{
    QSignalSpy spy(&b, &IRadioBackend::gpsChanged);
    b.decodeGpsStatus(body);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<GpsDelta>();
}

static MemoryDelta decodeMem(FlexBackend& b, int idx, const QMap<QString, QString>& kvs)
{
    QSignalSpy spy(&b, &IRadioBackend::memoryChanged);
    b.decodeMemoryStatus(idx, kvs);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<MemoryDelta>();
}

static ProfileDelta decodeProfile(FlexBackend& b, const QString& type, const QString& body)
{
    QSignalSpy spy(&b, &IRadioBackend::profileChanged);
    b.decodeProfileStatus(type, body);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<ProfileDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<GpsDelta>();
    qRegisterMetaType<MemoryDelta>();
    qRegisterMetaType<ProfileDelta>();
    FlexBackend b;

    // ---- GPS: '#'-tokenised, case-insensitive keys, numeric counts ----
    {
        const GpsDelta d = decodeGps(b, QStringLiteral(
            "lat=48.27#lon=-116.56#grid=DN18rg#altitude=644 m#TRACKED=16#"
            "visible=31#speed=0 kts#track=273.4#freq_error=0 ppb#"
            "status=Fine Lock#time=05:25:20Z"));
        CHECK(d.status.has_value() && *d.status == QStringLiteral("Fine Lock"));
        CHECK(d.tracked.has_value() && *d.tracked == 16);   // case-insensitive key
        CHECK(d.visible.has_value() && *d.visible == 31);
        CHECK(d.grid.has_value() && *d.grid == QStringLiteral("DN18rg"));
        CHECK(d.altitude.has_value() && *d.altitude == QStringLiteral("644 m"));
        CHECK(d.lat.has_value() && *d.lat == QStringLiteral("48.27"));
        CHECK(d.lon.has_value() && *d.lon == QStringLiteral("-116.56"));
        CHECK(d.time.has_value() && *d.time == QStringLiteral("05:25:20Z"));
        CHECK(d.speed.has_value() && *d.speed == QStringLiteral("0 kts"));
        CHECK(d.track.has_value() && *d.track == QStringLiteral("273.4"));
        CHECK(d.freqError.has_value() && *d.freqError == QStringLiteral("0 ppb"));
    }

    // ---- GPS: token with no '=' (or empty key) skipped ----
    {
        const GpsDelta d = decodeGps(b, QStringLiteral("status=OK#garbage#=novalue#tracked=3"));
        CHECK(d.status.has_value() && *d.status == QStringLiteral("OK"));
        CHECK(d.tracked.has_value() && *d.tracked == 3);
        CHECK(!d.grid.has_value());
    }

    // ---- Memory: full definition, text carried raw, numerics typed ----
    {
        const MemoryDelta d = decodeMem(b, 5, {
            {QStringLiteral("group"), QStringLiteral("HAM")},
            {QStringLiteral("name"), QStringLiteral("Repeater 1")},
            {QStringLiteral("freq"), QStringLiteral("146.94")},
            {QStringLiteral("mode"), QStringLiteral("FM")},
            {QStringLiteral("repeater"), QStringLiteral("SIMPLEX")},
            {QStringLiteral("repeater_offset"), QStringLiteral("0.6")},
            {QStringLiteral("squelch"), QStringLiteral("1")},
            {QStringLiteral("step"), QStringLiteral("5000")},
            {QStringLiteral("rtty_mark"), QStringLiteral("2125")},
        });
        CHECK(d.index == 5);
        CHECK(!d.removed);
        CHECK(d.group.has_value() && *d.group == QStringLiteral("HAM"));
        CHECK(d.name.has_value() && *d.name == QStringLiteral("Repeater 1"));  // raw (model sanitises)
        CHECK(d.freq.has_value() && qFuzzyCompare(*d.freq, 146.94));
        CHECK(d.mode.has_value() && *d.mode == QStringLiteral("FM"));
        CHECK(d.offsetDir.has_value() && *d.offsetDir == QStringLiteral("SIMPLEX"));  // wire key "repeater"
        CHECK(d.repeaterOffset.has_value() && qFuzzyCompare(*d.repeaterOffset, 0.6));
        CHECK(d.squelch.has_value() && *d.squelch == true);
        CHECK(d.step.has_value() && *d.step == 5000);
        CHECK(d.rttyMark.has_value() && *d.rttyMark == 2125);
        CHECK(!d.owner.has_value());  // absent → disengaged
    }

    // ---- Memory: removal via in_use=0 ----
    {
        const MemoryDelta d = decodeMem(b, 5, {{QStringLiteral("in_use"), QStringLiteral("0")}});
        CHECK(d.index == 5);
        CHECK(d.removed);
    }

    // ---- Memory: removal via bare "removed" key ----
    {
        const MemoryDelta d = decodeMem(b, 9, {{QStringLiteral("removed"), QString()}});
        CHECK(d.index == 9);
        CHECK(d.removed);
    }

    // ---- Memory: ok-guard — malformed present numeric dropped, not 0 ----
    {
        const MemoryDelta d = decodeMem(b, 3, {
            {QStringLiteral("freq"), QStringLiteral("junk")},
            {QStringLiteral("step"), QStringLiteral("nope")},
            {QStringLiteral("name"), QStringLiteral("Keep")},
        });
        CHECK(!d.freq.has_value());   // dropped, not 0.0 (fail-closed)
        CHECK(!d.step.has_value());   // dropped, not 0
        CHECK(d.name.has_value() && *d.name == QStringLiteral("Keep"));
    }

    // ---- Profile: tx list '^'-split (values may contain spaces) ----
    {
        const ProfileDelta d = decodeProfile(b, QStringLiteral("tx"),
            QStringLiteral("list=Default^Default FHM-1^Default FHM-1 DX"));
        CHECK(d.type == QStringLiteral("tx"));
        CHECK(d.list.has_value());
        CHECK(d.list->size() == 3);
        CHECK(d.list->at(1) == QStringLiteral("Default FHM-1"));  // space preserved
        CHECK(!d.current.has_value());
    }

    // ---- Profile: mic current (single value with spaces, trimmed) ----
    {
        const ProfileDelta d = decodeProfile(b, QStringLiteral("mic"),
            QStringLiteral("current= Default FHM-1 "));
        CHECK(d.type == QStringLiteral("mic"));
        CHECK(d.current.has_value() && *d.current == QStringLiteral("Default FHM-1"));  // trimmed
        CHECK(!d.list.has_value());
    }

    // ---- Profile: importing/exporting flags (type-independent early path) ----
    {
        const ProfileDelta d = decodeProfile(b, QStringLiteral("global"),
            QStringLiteral("importing=1"));
        CHECK(d.importing.has_value() && *d.importing == true);
        CHECK(!d.list.has_value() && !d.current.has_value());
    }

    // ---- Profile: unknown key for a type → no emission ----
    {
        QSignalSpy spy(&b, &IRadioBackend::profileChanged);
        b.decodeProfileStatus(QStringLiteral("tx"), QStringLiteral("bogus=1"));
        CHECK(spy.count() == 0);
    }

    // ---- Profile flags fallback: space-free kv-set ----
    {
        QSignalSpy spy(&b, &IRadioBackend::profileChanged);
        b.decodeProfileFlags({{QStringLiteral("exporting"), QStringLiteral("1")}});
        CHECK(spy.count() == 1);
        const ProfileDelta d = spy.takeFirst().at(0).value<ProfileDelta>();
        CHECK(d.exporting.has_value() && *d.exporting == true);

        QSignalSpy spy2(&b, &IRadioBackend::profileChanged);
        b.decodeProfileFlags({{QStringLiteral("unrelated"), QStringLiteral("x")}});
        CHECK(spy2.count() == 0);   // neither flag present → no-op
    }

    if (g_failures == 0) {
        std::printf("aetherd_residual_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_residual_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
