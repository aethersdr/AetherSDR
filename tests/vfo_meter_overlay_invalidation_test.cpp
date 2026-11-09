// A moving in-flag bar must not invalidate the spectrum's separate value-label
// overlay when there are no labels to draw. Clearing the last label must still
// invalidate immediately, even inside the animation notification throttle.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/MeterViewController.h"
#include "gui/SmartMtrWidget.h"
#include "gui/VfoWidget.h"
#include "models/MeterModel.h"
#include "models/SliceModel.h"
#include "core/backends/SliceDelta.h"
#include <QScopeGuard>
#include <QLabel>
#include <QImage>
#include <cmath>

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

    void radioDisplayDeflectionUsesNativeUnitsAndLinearBar()
    {
        MeterViewController::instance().setSmartMtr(false);
        VfoWidget vfo;
        vfo.show();
        DisplayDeflectionReading reading;
        reading.unit = QStringLiteral("dot");
        reading.low = 0;
        reading.high = 70;
        reading.description = QStringLiteral("Radio display deflection; uncalibrated");
        reading.value = 0;
        vfo.setDisplayDeflection(reading);
        QTest::qWait(60);
        QLabel* label = nullptr;
        for (QLabel* candidate : vfo.findChildren<QLabel*>()) {
            if (candidate->accessibleName() == QStringLiteral("Relative radio display deflection in dots")) {
                label = candidate;
                break;
            }
        }
        QVERIFY(label);
        QCOMPARE(label->text(), QStringLiteral("0 dots"));
        QCOMPARE(label->accessibleDescription(), reading.description);
        const auto barPixels = [&vfo, label]() {
            const QImage image = vfo.grab().toImage().scaled(
                vfo.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
            const int y = label->mapTo(&vfo, QPoint(0, label->height() / 2)).y();
            // Inspect only the left signal-bar portion; numeric label changes
            // cannot make this assertion pass when the fill is stuck.
            return image.copy(6, y, (vfo.width() - 12) * 3 / 4, 1);
        };
        const QImage empty = barPixels();
        reading.value = 70;
        vfo.setDisplayDeflection(reading);
        QTest::qWait(400);
        const QImage full = barPixels();
        reading.value = 35;
        vfo.setDisplayDeflection(reading);
        QTest::qWait(1400);
        const QImage half = barPixels();
        int fullPixels = 0;
        int halfPixels = 0;
        for (int x = 0; x < empty.width(); ++x) {
            fullPixels += full.pixel(x, 0) != empty.pixel(x, 0);
            halfPixels += half.pixel(x, 0) != empty.pixel(x, 0);
        }
        QVERIFY(fullPixels >= empty.width() - 2);
        QVERIFY(std::abs(2 * halfPixels - fullPixels) <= 4);
        QCOMPARE(label->text(), QStringLiteral("35 dots"));
        QVERIFY(!label->text().contains(QStringLiteral("dBm")));
        QVERIFY(!label->text().contains(QStringLiteral("dBFS")));

        reading.high = reading.low;
        vfo.setDisplayDeflection(reading);
        QCOMPARE(label->text(), QStringLiteral("— dots"));
        QCOMPARE(barPixels(), empty); // Invalid range cannot divide by zero or fill.
        reading.high = 70;
        reading.value.reset(); // The model's stale/removed sample representation.
        vfo.setDisplayDeflection(reading);
        QCOMPARE(label->text(), QStringLiteral("— dots"));
        QCOMPARE(barPixels(), empty);
        vfo.setDisplayDeflection(DisplayDeflectionReading{});
        QCOMPARE(label->text(), QStringLiteral("— dots"));
        QCOMPARE(barPixels(), empty);

        MeterViewController::instance().setSmartMtr(true);
        reading.value = 35;
        vfo.setDisplayDeflection(reading);
        SmartMtrWidget* smart = vfo.findChild<SmartMtrWidget*>();
        QVERIFY(smart);
        smart->grab();
        QVERIFY(smart->extremeLabels().isEmpty());
        QVERIFY(smart->accessibleDescription().contains(QStringLiteral("does not support")));
        QVERIFY(smart->accessibleDescription().contains(QStringLiteral("dots")));

        const DisplaySettings::TxMeter previousTxMeter = MeterViewController::instance().txMeter();
        const auto restoreTxMeter = qScopeGuard([previousTxMeter] {
            MeterViewController::instance().setTxMeter(previousTxMeter);
        });
        MeterViewController::instance().setTxMeter(DisplaySettings::TxMeter::None);
        vfo.setTransmitting(true);
        smart->grab();
        QVERIFY(smart->extremeLabels().isEmpty());
        QVERIFY(smart->accessibleDescription().contains(QStringLiteral("does not support")));
        // A transmitting flag with TX meter=None still selects the RX scale;
        // the raw sample must never be relabelled as ADC-relative dBFS.
        SliceModel slice(0);
        SliceDelta delta;
        delta.txSlice = true;
        slice.applyChanges(delta);
        vfo.setSlice(&slice);
        vfo.setDisplayDeflection(reading);
        smart->grab();
        QVERIFY(smart->extremeLabels().isEmpty());
        QVERIFY(smart->accessibleDescription().contains(QStringLiteral("does not support")));
        vfo.setTransmitting(false);
        vfo.setSlice(nullptr);

        // Switching an unavailable raw scale to unavailable ADC-relative RF
        // must still change the unit, even when both samples are absent.
        vfo.setDisplayDeflection(DisplayDeflectionReading{});
        vfo.setRelativeSignalLevel(std::nullopt);
        QVERIFY(!vfo.displayDeflection());
        QCOMPARE(label->text(), QStringLiteral("— dBFS"));
        vfo.setSignalLevel(-73);
        QVERIFY(!vfo.relativeSignalLevel());
        QCOMPARE(label->text(), QStringLiteral("-73 dBm"));
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
