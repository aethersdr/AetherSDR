// aetherd RFC 2.3 (RadioModel residual) — FlexBackend::decodeRadioStatus.
// Pins the radio-global wire → typed RadioDelta translation (key renames,
// "1"→bool, ok-guarded numerics, present-only) that moved out of
// RadioModel::handleRadioStatus.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/RadioDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static RadioDelta decode(FlexBackend& b, const QMap<QString, QString>& kvs)
{
    QSignalSpy spy(&b, &IRadioBackend::radioChanged);
    b.decodeRadioStatus(kvs);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(0).value<RadioDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<RadioDelta>();
    FlexBackend b;

    // ---- key renames + typed values + "1"→bool ----
    {
        const RadioDelta d = decode(b, {
            {QStringLiteral("model"), QStringLiteral("FLEX-8600")},
            {QStringLiteral("slices"), QStringLiteral("4")},
            {QStringLiteral("callsign"), QStringLiteral("KK7GWY")},
            {QStringLiteral("mf_enable"), QStringLiteral("1")},
            {QStringLiteral("full_duplex_enabled"), QStringLiteral("0")},
            {QStringLiteral("cal_freq"), QStringLiteral("10.0")},
            {QStringLiteral("rtty_mark_default"), QStringLiteral("2295")},
            {QStringLiteral("lineout_gain"), QStringLiteral("55")},
        });
        CHECK(d.model.has_value() && *d.model == QStringLiteral("FLEX-8600"));
        CHECK(d.slicesAvailable.has_value() && *d.slicesAvailable == 4);
        CHECK(d.callsign.has_value() && *d.callsign == QStringLiteral("KK7GWY"));
        CHECK(d.multiFlexEnabled.has_value() && *d.multiFlexEnabled == true);
        CHECK(d.fullDuplex.has_value() && *d.fullDuplex == false);
        CHECK(d.calFreqMhz.has_value() && qFuzzyCompare(*d.calFreqMhz, 10.0));
        CHECK(d.rttyMarkDefault.has_value() && *d.rttyMarkDefault == 2295);
        CHECK(d.lineoutGain.has_value() && *d.lineoutGain == 55);
        CHECK(!d.nickname.has_value());   // absent → disengaged
    }

    // ---- ok-guard: malformed present numeric dropped ----
    {
        const RadioDelta d = decode(b, {
            {QStringLiteral("slices"), QStringLiteral("junk")},
            {QStringLiteral("cal_freq"), QStringLiteral("nope")},
            {QStringLiteral("region"), QStringLiteral("US")},
        });
        CHECK(!d.slicesAvailable.has_value());  // dropped, not 0
        CHECK(!d.calFreqMhz.has_value());       // dropped, not 0.0
        CHECK(d.region.has_value() && *d.region == QStringLiteral("US"));
    }

    if (g_failures == 0) {
        std::printf("aetherd_radio_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_radio_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
