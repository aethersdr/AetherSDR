// The width -> passband rule, which used to live inside VfoWidget where nothing
// could reach it. Each clause here was written against a specific radio
// behaviour and several carry issue numbers; this pins them so the move out of
// the widget, and anything after it, has to keep them.

#include "gui/ModeFilterPresets.h"
#include "core/backends/RadioCapabilities.h"

#include <QtTest>

using namespace AetherSDR::ModeFilters;

class ModeFilterPresetsTest : public QObject {
    Q_OBJECT

private slots:
    void ssbPinsItsLowCutAndDerivesTheHigh();
    void cwCentresOnTheCarrier();
    void amAndFmStraddleIt_data();
    void amAndFmStraddleIt();
    void diguCentresOnItsOffsetAndClampsAt95();
    void rttyStraddlesMarkAndSpace();
    void laddersAreNarrowToWide();
    void unknownModesGetTheSsbLadder();
    void fmLaddersRequireDeclaredControl();
    void fmPresetsRespectDeclaredEdges();
};

void ModeFilterPresetsTest::ssbPinsItsLowCutAndDerivesTheHigh()
{
    // #3292: the label is the passband the operator gets. Sending lo=95,
    // hi=width gave 2805 Hz for the 2.9k preset and left the highlight
    // comparing against off-by-95 widths.
    const Edges usb = edgesForWidth(QStringLiteral("USB"), 2400, {});
    QCOMPARE(usb.lo, 100);
    QCOMPARE(usb.hi, 2500);
    QCOMPARE(widthForEdges(QStringLiteral("USB"), usb.lo, usb.hi), 2400);

    // LSB is the mirror: the edge nearest the carrier is -100.
    const Edges lsb = edgesForWidth(QStringLiteral("LSB"), 2400, {});
    QCOMPARE(lsb.hi, -100);
    QCOMPARE(lsb.lo, -2500);
}

void ModeFilterPresetsTest::cwCentresOnTheCarrier()
{
    // The radio's BFO handles the pitch offset, so the filter sits on the
    // carrier rather than on the tone.
    for (const char* mode : {"CW", "CWL", "CWU"}) {
        const Edges e = edgesForWidth(QString::fromLatin1(mode), 500, {});
        QCOMPARE(e.lo, -250);
        QCOMPARE(e.hi, 250);
    }
}

void ModeFilterPresetsTest::amAndFmStraddleIt_data()
{
    QTest::addColumn<QString>("mode");
    for (const char* mode : {"AM", "SAM", "DSB", "FM", "NFM", "FMN", "WFM", "DFM"}) {
        QTest::newRow(mode) << QString::fromLatin1(mode);
    }
}

void ModeFilterPresetsTest::amAndFmStraddleIt()
{
    QFETCH(QString, mode);
    const Edges e = edgesForWidth(mode, 8000, {});
    QCOMPARE(e.lo, -4000);
    QCOMPARE(e.hi, 4000);
}

void ModeFilterPresetsTest::diguCentresOnItsOffsetAndClampsAt95()
{
    SliceContext ctx;
    ctx.diguOffset = 1500;

    // Under 3 kHz the filter centres on the offset.
    const Edges narrow = edgesForWidth(QStringLiteral("DIGU"), 1000, ctx);
    QCOMPARE(narrow.lo, 1000);
    QCOMPARE(narrow.hi, 2000);

    // ...and slides up rather than dipping below 95 Hz, keeping its width.
    ctx.diguOffset = 300;
    const Edges clamped = edgesForWidth(QStringLiteral("DIGU"), 1000, ctx);
    QCOMPARE(clamped.lo, 95);
    QCOMPARE(clamped.hi - clamped.lo, 1000);

    // At 3 kHz and above the offset is ignored, as SmartSDR does.
    const Edges wide = edgesForWidth(QStringLiteral("DIGU"), 3000, ctx);
    QCOMPARE(wide.lo, 95);
    QCOMPARE(wide.hi, 3000);
}

void ModeFilterPresetsTest::rttyStraddlesMarkAndSpace()
{
    SliceContext ctx;
    ctx.rttyShift = 170;   // space sits 170 Hz below mark, which is at 0

    const Edges e = edgesForWidth(QStringLiteral("RTTY"), 500, ctx);
    QCOMPARE((e.lo + e.hi) / 2, -85);        // midway between the two tones
    QCOMPARE(e.hi - e.lo, 500);
    QVERIFY(e.lo < -170 && e.hi > 0);        // both tones inside the passband
}

void ModeFilterPresetsTest::laddersAreNarrowToWide()
{
    for (const char* mode : {"USB", "AM", "CW", "DIGU", "RTTY", "DFM"}) {
        const QVector<int>& ladder = widthsForMode(QString::fromLatin1(mode));
        QVERIFY2(!ladder.isEmpty(), mode);
        for (int i = 1; i < ladder.size(); ++i) {
            QVERIFY2(ladder[i] > ladder[i - 1], mode);
        }
    }
    // FM has no ladder: the radio does not offer one.
    QVERIFY(widthsForMode(QStringLiteral("FM")).isEmpty());
}

void ModeFilterPresetsTest::unknownModesGetTheSsbLadder()
{
    QCOMPARE(widthsForMode(QStringLiteral("NOT-A-MODE")),
             widthsForMode(QStringLiteral("USB")));
}

void ModeFilterPresetsTest::fmLaddersRequireDeclaredControl()
{
    const AetherSDR::ReceiveFilterControl control{
        AetherSDR::SliceFrequencyControl::Authority::Engine,
        {{QStringLiteral("FM"), -21600, -1, 1, 21600, 2, 43200},
         {QStringLiteral("FMN"), -21600, -1, 1, 21600, 2, 43200}}};
    for (const char* spelling : {"FM", "FMN", "NFM"}) {
        const QString mode = QString::fromLatin1(spelling);
        QVERIFY(widthsForMode(mode).isEmpty()); // unchanged fixed-FM default
        const QVector<int> widths = widthsForMode(mode, &control);
        QVERIFY(!widths.isEmpty());
        QVERIFY(widths.contains(16000));
        for (int width : widths) {
            const Edges edges = edgesForWidth(mode, width, {});
            QCOMPARE(edges.lo, -edges.hi);
            QVERIFY(acceptsFmEdges(mode, &control, edges));
        }
    }
    QVERIFY(widthsForMode(QStringLiteral("WFM"), &control).isEmpty());
    QVERIFY(!acceptsFmEdges(QStringLiteral("WFM"), &control, {-8000, 8000}));
    QCOMPARE(widthsForMode(QStringLiteral("USB"), &control),
             widthsForMode(QStringLiteral("USB")));
}

void ModeFilterPresetsTest::fmPresetsRespectDeclaredEdges()
{
    const AetherSDR::ReceiveFilterControl control{
        AetherSDR::SliceFrequencyControl::Authority::Engine,
        {{QStringLiteral("FMN"), -6000, -1, 1, 6000, 2, 12000}}};
    const QVector<int> expected{6000, 8000, 10000, 12000};
    QCOMPARE(widthsForMode(QStringLiteral("FMN"), &control), expected);
    QVERIFY(!acceptsFmEdges(QStringLiteral("FMN"), &control, {95, 8000}));
    QVERIFY(!acceptsFmEdges(QStringLiteral("FMN"), &control, {-8000, 8000}));
    QVERIFY(!acceptsFmEdges(QStringLiteral("FMN"), &control, {1000, -1000}));
    QVERIFY(acceptsFmEdges(QStringLiteral("FMN"), &control, {-4000, 6000}));
}

QTEST_MAIN(ModeFilterPresetsTest)
#include "mode_filter_presets_test.moc"
