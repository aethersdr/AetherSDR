#include "models/PanadapterModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <QVariantMap>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (a_ != e_) { \
        const QString a_str = QString("%1").arg(a_); \
        const QString e_str = QString("%1").arg(e_); \
        std::fprintf(stderr, "FAIL %s:%d  expected %s, got %s\n", \
                     __FILE__, __LINE__, \
                     e_str.toUtf8().constData(), \
                     a_str.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    PanadapterModel pan(QStringLiteral("0x40000000"));
    QSignalSpy spy(&pan, &PanadapterModel::rxAntennaChanged);

    // aetherd RFC 2.3: rxant/rfgain decode moved to FlexBackend; the model now
    // exposes normalized setRxAntenna/setRfGain. This test pins their change-
    // gating semantics (the wire→setter decode is covered by aetherd_pan_decode_test).
    pan.setRxAntenna(QStringLiteral("ANT1"));
    EXPECT_EQ(pan.rxAntenna(), QStringLiteral("ANT1"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("ANT1"));

    pan.setRxAntenna(QStringLiteral("ANT1"));
    EXPECT_EQ(spy.count(), 0);

    pan.setRxAntenna(QStringLiteral("RX_A"));
    EXPECT_EQ(pan.rxAntenna(), QStringLiteral("RX_A"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("RX_A"));

    QSignalSpy gainSpy(&pan, &PanadapterModel::rfGainChanged);
    pan.setRfGain(20);
    EXPECT_EQ(pan.rfGain(), 20);
    EXPECT_EQ(gainSpy.count(), 1);
    EXPECT_EQ(gainSpy.takeFirst().at(0).toInt(), 20);

    pan.setRfGain(20);
    EXPECT_EQ(gainSpy.count(), 0);

    pan.setRfGain(-8);
    EXPECT_EQ(pan.rfGain(), -8);
    EXPECT_EQ(gainSpy.count(), 1);
    EXPECT_EQ(gainSpy.takeFirst().at(0).toInt(), -8);

    // #4147: malformed bool wire flags must be ignored, keeping last-known-good
    // state — mirroring FlexLib's TryParse guard (Panadapter.cs). A garbled or
    // out-of-range value must NOT silently overwrite a prior true with false.
    {
        auto apply = [&pan](const QString& key, const QString& val) {
            QVariantMap m;
            m.insert(key, val);
            pan.applyStateExtension(m);
        };

        // wide: reject non-numeric and > 1, keep prior state.
        apply(QStringLiteral("wide"), QStringLiteral("1"));
        EXPECT_EQ(pan.wideActive(), true);
        apply(QStringLiteral("wide"), QStringLiteral("bogus"));
        EXPECT_EQ(pan.wideActive(), true);   // malformed → kept
        apply(QStringLiteral("wide"), QStringLiteral("2"));
        EXPECT_EQ(pan.wideActive(), true);   // out-of-range → kept
        apply(QStringLiteral("wide"), QStringLiteral("0"));
        EXPECT_EQ(pan.wideActive(), false);  // well-formed 0 → applied

        // loopa: same guard + range reject.
        apply(QStringLiteral("loopa"), QStringLiteral("1"));
        EXPECT_EQ(pan.loopA(), true);
        apply(QStringLiteral("loopa"), QStringLiteral("xx"));
        EXPECT_EQ(pan.loopA(), true);        // malformed → kept
        apply(QStringLiteral("loopa"), QStringLiteral("3"));
        EXPECT_EQ(pan.loopA(), true);        // out-of-range → kept

        // loopb: same; setting it clears loopa via mutual exclusion.
        apply(QStringLiteral("loopb"), QStringLiteral("1"));
        EXPECT_EQ(pan.loopB(), true);
        EXPECT_EQ(pan.loopA(), false);
        apply(QStringLiteral("loopb"), QStringLiteral("nope"));
        EXPECT_EQ(pan.loopB(), true);        // malformed → kept
        apply(QStringLiteral("loopb"), QStringLiteral("2"));
        EXPECT_EQ(pan.loopB(), true);        // out-of-range → kept

        // Mutual exclusion in the other direction: loopa=1 clears loopB.
        apply(QStringLiteral("loopa"), QStringLiteral("1"));
        EXPECT_EQ(pan.loopA(), true);
        EXPECT_EQ(pan.loopB(), false);

        // Well-formed 0 must still be accepted — an over-tightened guard that
        // rejects legitimate off transitions would leave the loop flag stuck.
        apply(QStringLiteral("loopa"), QStringLiteral("0"));
        EXPECT_EQ(pan.loopA(), false);
        apply(QStringLiteral("loopb"), QStringLiteral("1"));
        apply(QStringLiteral("loopb"), QStringLiteral("0"));
        EXPECT_EQ(pan.loopB(), false);

        // weighted_average: byte.TryParse semantics per FlexLib (Panadapter.cs
        // 1195-1208) — parse-guard, byte range 0-255, but NO > 1 reject: any
        // nonzero in-range value applies as true; negatives and > 255 fail the
        // byte parse and keep last-known-good.
        QSignalSpy reportedSpy(&pan, &PanadapterModel::weightedAverageReported);
        apply(QStringLiteral("weighted_average"), QStringLiteral("1"));
        EXPECT_EQ(pan.weightedAverage(), true);
        EXPECT_EQ(reportedSpy.count(), 1);
        apply(QStringLiteral("weighted_average"), QStringLiteral("garbage"));
        EXPECT_EQ(pan.weightedAverage(), true);  // malformed → kept
        EXPECT_EQ(reportedSpy.count(), 1);       // …and NOT reported (skip, not coerce)
        apply(QStringLiteral("weighted_average"), QStringLiteral("-1"));
        EXPECT_EQ(pan.weightedAverage(), true);  // fails byte parse → kept
        apply(QStringLiteral("weighted_average"), QStringLiteral("300"));
        EXPECT_EQ(pan.weightedAverage(), true);  // > 255 fails byte parse → kept
        EXPECT_EQ(reportedSpy.count(), 1);
        apply(QStringLiteral("weighted_average"), QStringLiteral("0"));
        EXPECT_EQ(pan.weightedAverage(), false); // well-formed 0 → applied
        EXPECT_EQ(reportedSpy.count(), 2);
        apply(QStringLiteral("weighted_average"), QStringLiteral("2"));
        EXPECT_EQ(pan.weightedAverage(), true);  // in byte range, no > 1 reject → true
        EXPECT_EQ(reportedSpy.count(), 3);

        // daxiq_channel: uint parse + skip on failure (Panadapter.cs 980-994) —
        // a garbled value must not rebind the pan to channel 0.
        apply(QStringLiteral("daxiq_channel"), QStringLiteral("2"));
        EXPECT_EQ(pan.daxiqChannel(), 2);
        apply(QStringLiteral("daxiq_channel"), QStringLiteral("junk"));
        EXPECT_EQ(pan.daxiqChannel(), 2);        // malformed → kept
        apply(QStringLiteral("daxiq_channel"), QStringLiteral("-1"));
        EXPECT_EQ(pan.daxiqChannel(), 2);        // fails uint parse → kept
        apply(QStringLiteral("daxiq_channel"), QStringLiteral("0"));
        EXPECT_EQ(pan.daxiqChannel(), 0);        // well-formed 0 → applied

        // waterfall: validated before storing (Panadapter.cs 1177-1193) —
        // garbage must not displace a valid stream id (wfStreamId pairing and
        // outgoing `display panafall set <id>` both read it back).
        apply(QStringLiteral("waterfall"), QStringLiteral("0x42000008"));
        EXPECT_EQ(pan.waterfallId(), QStringLiteral("0x42000008"));
        apply(QStringLiteral("waterfall"), QStringLiteral("garbage"));
        EXPECT_EQ(pan.waterfallId(), QStringLiteral("0x42000008"));  // kept
        apply(QStringLiteral("waterfall"), QStringLiteral("4200000f"));
        EXPECT_EQ(pan.waterfallId(), QStringLiteral("4200000f"));    // bare hex accepted
    }

    if (g_failures == 0) {
        std::printf("panadapter_model_rx_antenna_test: all checks passed\n");
        return 0;
    }
    std::printf("panadapter_model_rx_antenna_test: %d failure(s)\n", g_failures);
    return 1;
}
