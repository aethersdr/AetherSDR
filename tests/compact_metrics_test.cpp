// CompactMetrics shrinks a panel's graphics so it fits a small window without
// touching its text. The rules it applies are invisible at runtime -- a knob
// that is 8 px too narrow does not warn, it just serves "-40.0 dB" as
// "40.0 dB" -- so the exclusions are asserted here rather than discovered on
// screen later.

#include "gui/CompactMetrics.h"

#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QtTest>

using namespace AetherSDR;

namespace {

// Stand-ins for the real controls: CompactMetrics recognises a knob and a
// meter by class name, because they come from several unrelated widget
// families and what they share is printed text, not an ancestor.
class FakeKnob : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class FakeMeter : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
};

class FakeCurve : public QWidget {
    Q_OBJECT
public:
    using QWidget::QWidget;
};

} // namespace

class CompactMetricsTest : public QObject {
    Q_OBJECT

private slots:
    void knobsAndTextAreNeverTouched();
    void metersKeepTheirWidthAndGiveUpHeight();
    void containersOfTextAreLeftAlone();
    void plainGraphicsShrink();
    void applyIsAbsoluteNotCumulative();
    void nothingShrinksBelowTheFloor();
};

void CompactMetricsTest::knobsAndTextAreNeverTouched()
{
    QWidget root;
    auto* knob = new FakeKnob(&root);
    knob->setFixedSize(76, 76);
    auto* label = new QLabel(&root);
    label->setFixedWidth(120);
    auto* button = new QPushButton(&root);
    button->setFixedHeight(24);

    CompactMetrics metrics(&root);
    metrics.apply(0.6);

    // The knob's value editor is 11 px text across a 76 px widget; there is
    // nothing to give.
    QCOMPARE(knob->size(), QSize(76, 76));
    // A text widget's size IS its font.
    QCOMPARE(label->width(), 120);
    QCOMPARE(button->height(), 24);
}

void CompactMetricsTest::metersKeepTheirWidthAndGiveUpHeight()
{
    QWidget root;
    auto* meter = new FakeMeter(&root);
    meter->setFixedSize(42, 200);

    CompactMetrics metrics(&root);
    metrics.apply(0.6);

    QCOMPARE(meter->width(), 42);              // scale figures live here
    QVERIFY(meter->height() < 200);            // the bar itself does not
}

void CompactMetricsTest::containersOfTextAreLeftAlone()
{
    QWidget root;
    auto* readoutRow = new FakeCurve(&root);   // a graphic by class...
    readoutRow->setFixedHeight(58);
    auto* value = new QLabel(readoutRow);      // ...holding stacked readings
    value->setText(QStringLiteral("1.50 kHz"));

    CompactMetrics metrics(&root);
    metrics.apply(0.6);

    // 58 * 0.6 is 35, which slices through the middle of three stacked lines.
    QCOMPARE(readoutRow->height(), 58);
}

void CompactMetricsTest::plainGraphicsShrink()
{
    QWidget root;
    auto* curve = new FakeCurve(&root);
    curve->setMinimumHeight(180);
    auto* fixed = new FakeCurve(&root);
    fixed->setFixedSize(100, 100);

    CompactMetrics metrics(&root);
    metrics.apply(0.75);

    QCOMPARE(curve->minimumHeight(), 135);
    QCOMPARE(fixed->size(), QSize(75, 75));

    // Asking for less than kMinFactor gets kMinFactor: past that a curve is
    // a smudge and a knob is a dot, and the page is better off admitting it
    // does not fit than rendering something unreadable.
    metrics.apply(0.2);
    QCOMPARE(curve->minimumHeight(), qRound(180 * CompactMetrics::kMinFactor));
}

void CompactMetricsTest::applyIsAbsoluteNotCumulative()
{
    QWidget root;
    auto* curve = new FakeCurve(&root);
    curve->setFixedSize(200, 200);

    CompactMetrics metrics(&root);
    metrics.apply(0.8);
    QCOMPARE(curve->size(), QSize(160, 160));

    // Applied to the captured original, not to the last result -- 0.8 twice
    // over would leave 128 and the page would creep smaller every resize.
    metrics.apply(0.8);
    QCOMPARE(curve->size(), QSize(160, 160));

    metrics.apply(1.0);
    QCOMPARE(curve->size(), QSize(200, 200));
}

void CompactMetricsTest::nothingShrinksBelowTheFloor()
{
    QWidget root;
    auto* tiny = new FakeCurve(&root);
    tiny->setFixedSize(30, 30);
    auto* alreadySmall = new FakeCurve(&root);
    alreadySmall->setFixedSize(12, 12);

    CompactMetrics metrics(&root);
    metrics.apply(CompactMetrics::kMinFactor);

    QCOMPARE(tiny->width(), CompactMetrics::kFloorPx);
    // Something that started under the floor is left where it was rather than
    // being grown up to it.
    QCOMPARE(alreadySmall->width(), 12);
}

QTEST_MAIN(CompactMetricsTest)
#include "compact_metrics_test.moc"
