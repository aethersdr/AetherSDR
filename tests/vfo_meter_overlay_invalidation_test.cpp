// A moving in-flag bar must not invalidate the spectrum's separate value-label
// overlay when there are no labels to draw. Clearing the last label must still
// invalidate immediately, even inside the animation notification throttle.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/MeterViewController.h"
#include "gui/SmartMtrLabelStrip.h"
#include "gui/SmartMtrWidget.h"
#include "gui/VfoWidget.h"

#include <QApplication>
#include <QSignalSpy>
#include <QPainter>
#include <QFontMetricsF>
#include <QtTest>

using namespace AetherSDR;

class PaintCountingVfo : public VfoWidget
{
public:
    int paints = 0;
protected:
    void paintEvent(QPaintEvent* event) override
    {
        ++paints;
        VfoWidget::paintEvent(event);
    }
};

class VfoMeterOverlayInvalidationTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        MeterViewController::instance().setSmartMtr(true);
        MeterViewController::instance().setShowExtremes(false);
        MeterViewController::instance().setShowValues(DisplaySettings::MeterValues::None);
    }

    void barWithoutValueLabelsDoesNotInvalidateOverlay_data()
    {
        QTest::addColumn<bool>("relative");
        QTest::newRow("calibrated") << false;
        QTest::newRow("relative-dBFS") << true;
    }

    void barWithoutValueLabelsDoesNotInvalidateOverlay()
    {
        QFETCH(bool, relative);
        VfoWidget vfo;
        vfo.show();
        SmartMtrWidget* meter = vfo.findChild<SmartMtrWidget*>();
        QVERIFY(meter);
        QTest::qWait(60);
        QSignalSpy invalidations(&vfo, &VfoWidget::smartMtrLabelsChanged);
        const auto setLevel = [&vfo, relative](float level) {
            if (relative) {
                vfo.setRelativeSignalLevel(level);
            } else {
                vfo.setSignalLevel(level);
            }
        };
        setLevel(-100);
        QTest::qWait(200);
        const QImage low = meter->grab().toImage();
        setLevel(-20);
        QTest::qWait(200);
        const QImage high = meter->grab().toImage();
        QVERIFY(!low.isNull());
        QVERIFY(low != high); // the real bar still animates/renders
        QVERIFY(meter->extremeLabels().isEmpty());
        QVERIFY2(invalidations.isEmpty(), "bar-only paints must not rebuild the spectrum label overlay");
    }

    void valueLabelsStillUpdateAndClearWithoutWaitingForThrottle()
    {
        MeterViewController::instance().setShowValues(DisplaySettings::MeterValues::Signal);
        VfoWidget vfo;
        vfo.show();
        SmartMtrWidget* meter = vfo.findChild<SmartMtrWidget*>();
        QVERIFY(meter);
        QSignalSpy invalidations(&vfo, &VfoWidget::smartMtrLabelsChanged);
        vfo.setRelativeSignalLevel(-60);
        QTest::qWait(60);
        meter->grab();
        QVERIFY(!invalidations.isEmpty());
        QCOMPARE(meter->extremeLabels().size(), 1);
        QVERIFY(meter->extremeLabels().front().primary.contains(QStringLiteral("dBFS")));

        // The immediately following disappearance must erase the overlay even
        // when the preceding paint has just consumed the normal throttle slot.
        invalidations.clear();
        vfo.setRelativeSignalLevel(std::nullopt);
        meter->grab();
        QCOMPARE(invalidations.size(), 1);
        QVERIFY(meter->extremeLabels().isEmpty());
        invalidations.clear();
        QTest::qWait(60);
        meter->grab();
        QVERIFY(invalidations.isEmpty()); // no repeated clear redraws

        vfo.setRelativeSignalLevel(-40);
        QTest::qWait(60);
        meter->grab();
        QVERIFY(!invalidations.isEmpty());
        QCOMPARE(meter->extremeLabels().size(), 1);
        invalidations.clear();
        vfo.setSignalLevel(-73);
        QTest::qWait(60);
        meter->grab();
        QVERIFY(!invalidations.isEmpty());
        QVERIFY(meter->extremeLabels().front().secondary.contains(QStringLiteral("dBm")));
    }

    void unavailableRelativeMetersStayQuietAndRecover()
    {
        PaintCountingVfo vfo;
        vfo.show();
        SmartMtrWidget* meter = vfo.findChild<SmartMtrWidget*>();
        QVERIFY(meter);
        vfo.setRelativeSignalLevel(-20);
        QTest::qWait(100);
        const QImage live = meter->grab().toImage();
        vfo.setRelativeSignalLevel(std::nullopt, QStringLiteral("Receive meters disabled"));
        QTest::qWait(60);
        const QImage off = meter->grab().toImage();
        QVERIFY(live != off);
        QCOMPARE(meter->accessibleDescription(), QStringLiteral("Receive meters disabled"));
        QVERIFY(meter->extremeLabels().isEmpty());
        QTest::qWait(60);
        vfo.paints = 0;
        for (int i = 0; i < 4; ++i) {
            vfo.setRelativeSignalLevel(std::nullopt, QStringLiteral("Receive meters disabled"));
            QTest::qWait(50);
        }
        QCOMPARE(vfo.paints, 0);
        vfo.setRelativeSignalLevel(-40);
        QTest::qWait(100);
        QVERIFY(meter->grab().toImage() != off);
        QVERIFY(meter->accessibleDescription().isEmpty());
    }

    void inactiveStandardMeterDoesNotAnimateParent()
    {
        PaintCountingVfo vfo;
        vfo.show();
        SmartMtrWidget* meter = vfo.findChild<SmartMtrWidget*>();
        QVERIFY(meter);
        // Isolate parent repaints from the visible child's legitimate animation.
        meter->hide();
        vfo.setRelativeSignalLevel(-10);
        QTest::qWait(300);
        vfo.setRelativeSignalLevel(-110);
        QTest::qWait(60);
        vfo.paints = 0;
        QTest::qWait(200);
        QCOMPARE(vfo.paints, 0);

        MeterViewController::instance().setSmartMtr(false);
        QTest::qWait(60);
        vfo.setRelativeSignalLevel(-10);
        QTest::qWait(60);
        vfo.paints = 0;
        vfo.setRelativeSignalLevel(-110);
        QTest::qWait(200);
        QVERIFY(vfo.paints > 1); // the selected standard meter still animates

        // Switching away must stop an already-running standard animation too.
        MeterViewController::instance().setSmartMtr(true);
        meter->hide();
        QTest::qWait(60);
        vfo.paints = 0;
        QTest::qWait(200);
        QCOMPARE(vfo.paints, 0);
    }

    void smartMtrStripUploadMatchesFullFrame_data()
    {
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<int>("flagX");
        QTest::addColumn<int>("flagY");
        QTest::newRow("logical-left") << qreal(1.0) << 210 << 65;
        QTest::newRow("logical-right") << qreal(1.0) << 1740 << 66;
        QTest::newRow("fractional-125-left") << qreal(1.25) << 210 << 65;
        QTest::newRow("fractional-125-right") << qreal(1.25) << 1740 << 66;
        QTest::newRow("fractional-150-left") << qreal(1.5) << 210 << 65;
        QTest::newRow("fractional-150-right") << qreal(1.5) << 1740 << 66;
        QTest::newRow("retina-right") << qreal(2.0) << 1740 << 66;
    }

    void smartMtrStripUploadMatchesFullFrame()
    {
        QFETCH(qreal, dpr);
        QFETCH(int, flagX);
        QFETCH(int, flagY);
        MeterViewController::instance().setShowValues(
            DisplaySettings::MeterValues::Signal);
        QWidget spectrum;
        spectrum.resize(1900, 1154);
        VfoWidget flag(&spectrum);
        flag.move(flagX, flagY);
        spectrum.show();
        flag.show();
        SmartMtrWidget* meter = flag.findChild<SmartMtrWidget*>();
        QVERIFY(meter);
        flag.setRelativeSignalLevel(-75);
        // The labels refresh when the meter paints, so poll through grab().
        QTRY_VERIFY((meter->grab(), !meter->extremeLabels().isEmpty()));

        const QRect logicalCoverage = flag.smartMtrLabelsCoverageRect();
        QVERIFY2(!logicalCoverage.isEmpty(), "live labels require a redraw band");
        const QSize framebuffer(
            qCeil(spectrum.width() * dpr),
            qCeil(spectrum.height() * dpr));
        QImage background(framebuffer, QImage::Format_RGBA8888_Premultiplied);
        background.setDevicePixelRatio(dpr);
        background.fill(QColor(20, 33, 46));
        {
            QPainter p(&background);
            p.setPen(QPen(QColor(60, 180, 210), 1));
            for (int x = 0; x < spectrum.width(); x += 29)
                p.drawLine(x, 0, x, spectrum.height());
            p.fillRect(0, 100, spectrum.width(), 17, QColor(12, 19, 30));
        }

        // The same band, restore and clip that SpectrumWidget::renderGpuFrame
        // uses for a SmartMTR-only refresh.
        const QRect strip = SmartMtrLabelStrip::devicePixels(
            logicalCoverage, dpr, background.rect());
        QVERIFY(!strip.isEmpty());
        QVERIFY(strip.height() < framebuffer.height() / 4);
        // Independent of the helper: the band rounds outwards to whole pixels.
        QCOMPARE(strip, QRectF(logicalCoverage.x() * dpr, logicalCoverage.y() * dpr,
                               logicalCoverage.width() * dpr,
                               logicalCoverage.height() * dpr)
                            .toAlignedRect()
                            .intersected(background.rect()));
        const QImage savedBackgroundStrip = background.copy(strip);

        const auto withLabels = [&](const QImage& base) {
            QImage image = base.copy();
            QPainter p(&image);
            flag.drawSmartMtrLabels(p);
            return image;
        };
        const QImage first = withLabels(background);

        // Cursor-frequency chrome and the opaque dBm axis are foreground,
        // not part of the cached pre-label strip. A narrow label refresh must
        // repaint their occlusion in the same order as a complete frame.
        const QColor foregroundColor(7, 18, 33);
        const auto withForeground = [&](QImage image) {
            QPainter painter(&image);
            painter.fillRect(logicalCoverage, foregroundColor);
            return image;
        };
        const QImage firstWithForeground = withForeground(first);

        // Moving values must replace old glyphs, not blend over them.
        flag.setRelativeSignalLevel(-15);
        QTRY_VERIFY2((meter->grab(), withLabels(background) != first),
                     "value change must move/redraw labels");
        QCOMPARE(flag.smartMtrLabelsCoverageRect(), logicalCoverage);
        const QImage expected = withLabels(background);
        QImage partial = first.copy();
        QVERIFY(SmartMtrLabelStrip::restore(partial, savedBackgroundStrip, strip));
        partial = withLabels(partial);
        QCOMPARE(partial, expected);

        QImage clippedPartial = first.copy();
        QVERIFY(SmartMtrLabelStrip::repaint(
            clippedPartial, savedBackgroundStrip, strip, dpr,
            [&](QPainter& painter) { flag.drawSmartMtrLabels(painter); }));
        QCOMPARE(clippedPartial, expected);

        const QImage fullForeground = withForeground(expected);
        QImage missingForeground = firstWithForeground.copy();
        QVERIFY(SmartMtrLabelStrip::repaint(
            missingForeground, savedBackgroundStrip, strip, dpr,
            [&](QPainter& painter) { flag.drawSmartMtrLabels(painter); }));
        QVERIFY2(missingForeground != fullForeground,
                 "missing foreground repaint must change the finished pixels");
        QImage partialForeground = firstWithForeground.copy();
        QVERIFY(SmartMtrLabelStrip::repaint(
            partialForeground, savedBackgroundStrip, strip, dpr,
            [&](QPainter& painter) {
                flag.drawSmartMtrLabels(painter);
                painter.fillRect(logicalCoverage, foregroundColor);
            }));
        QCOMPARE(partialForeground, fullForeground);

        // A stale strip (wrong size) is refused, leaving the image untouched.
        QImage untouched = first.copy();
        QVERIFY(!SmartMtrLabelStrip::restore(
            untouched, savedBackgroundStrip.copy(0, 0, strip.width(),
                                                 strip.height() - 1),
            strip));
        QCOMPARE(untouched, first);

        // Clearing values may shrink the coverage to empty. A full rebuild
        // must then exactly erase the old text, including at fractional DPR.
        flag.setRelativeSignalLevel(std::nullopt);
        meter->grab();
        QVERIFY(meter->extremeLabels().isEmpty());
        QVERIFY(flag.smartMtrLabelsCoverageRect().isEmpty());
        QCOMPARE(withLabels(background), background);
    }

    void subpixelStandardMeterChangesDoNotRepaintTheCard()
    {
        MeterViewController::instance().setSmartMtr(false);
        PaintCountingVfo vfo;
        vfo.show();
        vfo.setSignalLevel(-80.25f);
        QTest::qWait(1000); // allow the original attack/release ballistics to settle
        const QImage settled = vfo.grab().toImage();
        vfo.paints = 0;
        for (float level : {-80.249f, -80.248f, -80.247f}) {
            vfo.setSignalLevel(level);
            QTest::qWait(30);
        }
        QCOMPARE(vfo.paints, 0);
        QCOMPARE(vfo.grab().toImage(), settled);

        vfo.setSignalLevel(-40.0f);
        QTest::qWait(200);
        QVERIFY(vfo.grab().toImage() != settled); // visible changes still animate
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vfo-meter-overlay-invalidation"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    VfoMeterOverlayInvalidationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "vfo_meter_overlay_invalidation_test.moc"
