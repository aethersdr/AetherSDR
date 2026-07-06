// aetherd RFC 2.3 — SliceModel touchpoint: FlexBackend::decodeSliceStatus.
// Pins the Flex slice-status wire → typed SliceDelta translation that moved out
// of SliceModel::applyStatus (key renames, "1"→bool, list split, lowercase,
// ok-guarded numeric parses, present-only). The model's apply-side behavior is
// covered by slice_model_letter_test / antenna_alias_test.

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/SliceDelta.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

// Decode one kv-set and return the emitted typed delta.
static SliceDelta decode(FlexBackend& b, const QMap<QString, QString>& kvs)
{
    QSignalSpy spy(&b, &IRadioBackend::sliceChanged);
    b.decodeSliceStatus(3, kvs);
    if (spy.count() != 1) return {};
    return spy.takeFirst().at(1).value<SliceDelta>();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qRegisterMetaType<SliceDelta>();
    FlexBackend b;

    // ---- key renames + typed values ----
    {
        const SliceDelta d = decode(b, {
            {QStringLiteral("RF_frequency"), QStringLiteral("14.25")},
            {QStringLiteral("mode"), QStringLiteral("USB")},
            {QStringLiteral("filter_lo"), QStringLiteral("-2700")},
            {QStringLiteral("filter_hi"), QStringLiteral("0")},
            {QStringLiteral("index_letter"), QStringLiteral("A")},
            {QStringLiteral("dax"), QStringLiteral("2")},
            {QStringLiteral("audio_level"), QStringLiteral("60")},
            {QStringLiteral("rfgain"), QStringLiteral("-5")},
        });
        CHECK(d.frequency.has_value() && qFuzzyCompare(*d.frequency, 14.25));
        CHECK(d.mode.has_value() && *d.mode == QStringLiteral("USB"));
        CHECK(d.filterLow.has_value() && *d.filterLow == -2700);
        CHECK(d.filterHigh.has_value() && *d.filterHigh == 0);
        CHECK(d.letter.has_value() && *d.letter == QStringLiteral("A"));
        CHECK(d.daxChannel.has_value() && *d.daxChannel == 2);
        CHECK(d.audioGain.has_value() && qFuzzyCompare(*d.audioGain, 60.0));
        CHECK(d.rfGain.has_value() && qFuzzyCompare(*d.rfGain, -5.0));
        // Fields not on the wire stay disengaged:
        CHECK(!d.qsk.has_value());
        CHECK(!d.txSlice.has_value());
    }

    // ---- "1"→bool; absent → disengaged ----
    {
        const SliceDelta d = decode(b, {
            {QStringLiteral("active"), QStringLiteral("1")},
            {QStringLiteral("tx"), QStringLiteral("0")},
            {QStringLiteral("lock"), QStringLiteral("1")},
        });
        CHECK(d.active.has_value() && *d.active == true);
        CHECK(d.txSlice.has_value() && *d.txSlice == false);
        CHECK(d.locked.has_value() && *d.locked == true);
        CHECK(!d.qsk.has_value());
    }

    // ---- ok-guarded numeric parse: a malformed present field is DROPPED (#4068) ----
    {
        // A garbled RF_frequency must NOT retune to 0 Hz — the field is dropped.
        const SliceDelta d = decode(b, {
            {QStringLiteral("RF_frequency"), QStringLiteral("garbage")},
            {QStringLiteral("filter_lo"), QStringLiteral("notanint")},
            {QStringLiteral("mode"), QStringLiteral("LSB")},
        });
        CHECK(!d.frequency.has_value());   // dropped, not 0.0
        CHECK(!d.filterLow.has_value());   // dropped, not 0
        CHECK(d.mode.has_value() && *d.mode == QStringLiteral("LSB"));  // valid field still carried
    }

    // ---- esc "1"/"on" → true, "0" → false ----
    {
        CHECK(*decode(b, {{QStringLiteral("esc"), QStringLiteral("on")}}).esc == true);
        CHECK(*decode(b, {{QStringLiteral("esc"), QStringLiteral("1")}}).esc == true);
        CHECK(*decode(b, {{QStringLiteral("esc"), QStringLiteral("0")}}).esc == false);
    }

    // ---- antenna lists: rx_ant_list precedence, split+trim; mode_list split ----
    {
        const SliceDelta d = decode(b, {
            {QStringLiteral("rx_ant_list"), QStringLiteral("ANT1, RX_A ,RX_B")},
            {QStringLiteral("ant_list"), QStringLiteral("SHOULD_BE_IGNORED")},
            {QStringLiteral("mode_list"), QStringLiteral("USB,LSB,CW")},
        });
        CHECK(d.rxAntennaList.has_value()
              && *d.rxAntennaList == QStringList({QStringLiteral("ANT1"),
                                                  QStringLiteral("RX_A"),
                                                  QStringLiteral("RX_B")}));
        CHECK(d.modeList.has_value() && d.modeList->size() == 3);
    }

    // ---- lowercase normalization; play/step_list carried raw ----
    {
        const SliceDelta d = decode(b, {
            {QStringLiteral("fm_tone_mode"), QStringLiteral("CTCSS")},
            {QStringLiteral("play"), QStringLiteral("disabled")},
            {QStringLiteral("step_list"), QStringLiteral("10,100,1000")},
        });
        CHECK(d.fmToneMode.has_value() && *d.fmToneMode == QStringLiteral("ctcss"));
        CHECK(d.play.has_value() && *d.play == QStringLiteral("disabled"));
        CHECK(d.stepList.has_value() && *d.stepList == QStringLiteral("10,100,1000"));
    }

    if (g_failures == 0) {
        std::printf("aetherd_slice_decode_test: all checks passed\n");
        return 0;
    }
    std::printf("aetherd_slice_decode_test: %d failure(s)\n", g_failures);
    return 1;
}
