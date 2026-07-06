// aetherd RFC 2.3 — SliceModel touchpoint: FlexBackend::decodeSliceStatus.
// Pins the Flex slice-status wire → canonical change-map translation that moved
// out of SliceModel::applyStatus (key renames, "1"→bool, list split, lowercase,
// present-only). The model's apply-side behavior is covered by
// slice_model_letter_test / antenna_alias_test.

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

// Decode one kv-set and return the emitted canonical change map.
static QVariantMap decode(FlexBackend& b, const QMap<QString, QString>& kvs)
{
    QSignalSpy spy(&b, &IRadioBackend::sliceChanged);
    b.decodeSliceStatus(3, kvs);
    if (spy.count() != 1) return {};
    const QList<QVariant> a = spy.takeFirst();
    return a.at(1).toMap();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    FlexBackend b;

    // ---- key renames + typed values ----
    {
        const QVariantMap c = decode(b, {
            {QStringLiteral("RF_frequency"), QStringLiteral("14.25")},
            {QStringLiteral("mode"), QStringLiteral("USB")},
            {QStringLiteral("filter_lo"), QStringLiteral("-2700")},
            {QStringLiteral("filter_hi"), QStringLiteral("0")},
            {QStringLiteral("index_letter"), QStringLiteral("A")},
            {QStringLiteral("dax"), QStringLiteral("2")},
            {QStringLiteral("audio_level"), QStringLiteral("60")},
            {QStringLiteral("rfgain"), QStringLiteral("-5")},
        });
        CHECK(qFuzzyCompare(c.value(QStringLiteral("frequency")).toDouble(), 14.25));
        CHECK(c.value(QStringLiteral("mode")).toString() == QStringLiteral("USB"));
        CHECK(c.value(QStringLiteral("filterLow")).toInt() == -2700);
        CHECK(c.value(QStringLiteral("filterHigh")).toInt() == 0);
        CHECK(c.value(QStringLiteral("letter")).toString() == QStringLiteral("A"));
        CHECK(c.value(QStringLiteral("daxChannel")).toInt() == 2);
        CHECK(qFuzzyCompare(c.value(QStringLiteral("audioGain")).toDouble(), 60.0));
        CHECK(c.value(QStringLiteral("rfGain")).toInt() == -5);
        // No RF_frequency (wire) key leaks through:
        CHECK(!c.contains(QStringLiteral("RF_frequency")));
    }

    // ---- "1"→bool, and absent keys not carried ----
    {
        const QVariantMap c = decode(b, {
            {QStringLiteral("active"), QStringLiteral("1")},
            {QStringLiteral("tx"), QStringLiteral("0")},
            {QStringLiteral("lock"), QStringLiteral("1")},
        });
        CHECK(c.value(QStringLiteral("active")).toBool() == true);
        CHECK(c.contains(QStringLiteral("txSlice")));
        CHECK(c.value(QStringLiteral("txSlice")).toBool() == false);
        CHECK(c.value(QStringLiteral("locked")).toBool() == true);
        CHECK(!c.contains(QStringLiteral("qsk")));   // absent → not carried
    }

    // ---- esc "1"/"on" → true, "0" → false ----
    {
        CHECK(decode(b, {{QStringLiteral("esc"), QStringLiteral("on")}})
                  .value(QStringLiteral("esc")).toBool() == true);
        CHECK(decode(b, {{QStringLiteral("esc"), QStringLiteral("1")}})
                  .value(QStringLiteral("esc")).toBool() == true);
        CHECK(decode(b, {{QStringLiteral("esc"), QStringLiteral("0")}})
                  .value(QStringLiteral("esc")).toBool() == false);
    }

    // ---- antenna lists: rx_ant_list precedence, split+trim; mode_list split ----
    {
        const QVariantMap c = decode(b, {
            {QStringLiteral("rx_ant_list"), QStringLiteral("ANT1, RX_A ,RX_B")},
            {QStringLiteral("ant_list"), QStringLiteral("SHOULD_BE_IGNORED")},
            {QStringLiteral("mode_list"), QStringLiteral("USB,LSB,CW")},
        });
        CHECK(c.value(QStringLiteral("rxAntennaList")).toStringList()
                  == QStringList({QStringLiteral("ANT1"), QStringLiteral("RX_A"), QStringLiteral("RX_B")}));
        CHECK(c.value(QStringLiteral("modeList")).toStringList().size() == 3);
    }

    // ---- lowercase normalization; play/step_list carried raw ----
    {
        const QVariantMap c = decode(b, {
            {QStringLiteral("fm_tone_mode"), QStringLiteral("CTCSS")},
            {QStringLiteral("play"), QStringLiteral("disabled")},
            {QStringLiteral("step_list"), QStringLiteral("10,100,1000")},
        });
        CHECK(c.value(QStringLiteral("fmToneMode")).toString() == QStringLiteral("ctcss"));
        CHECK(c.value(QStringLiteral("play")).toString() == QStringLiteral("disabled"));
        CHECK(c.value(QStringLiteral("stepList")).toString() == QStringLiteral("10,100,1000"));
    }

    if (g_failures == 0) {
        std::printf("aetherd_slice_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_slice_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
