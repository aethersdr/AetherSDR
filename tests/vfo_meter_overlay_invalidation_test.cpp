// A moving in-flag bar must not invalidate the spectrum's separate value-label
// overlay when there are no labels to draw. Clearing the last label must still
// invalidate immediately, even inside the animation notification throttle.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/MeterViewController.h"
#include "gui/SmartMtrWidget.h"
#include "gui/VfoWidget.h"

#include <QApplication>
#include <QSignalSpy>
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
