#include "gui/AetherLabWidget.h"
#include "models/AntennaAnalyzerModel.h"

#include <QAbstractSpinBox>
#include <QAccessible>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QScrollArea>
#include <QSignalSpy>
#include <QtTest>

namespace AetherSDR {
class AntennaAnalyzerModelTestAccess {
public:
    static void settled(AntennaAnalyzerModel& model)
    {
        model.m_identityQuiet.stop();
        QMetaObject::invokeMethod(&model.m_identityQuiet, "timeout", Qt::DirectConnection);
    }
};
}
using namespace AetherSDR;

class WindowInput final : public RigExpertHidTransport {
public:
    using RigExpertHidTransport::RigExpertHidTransport;
    QByteArray final;
    void start(const QString&) override { emit opened(); }
    void send(const QByteArray& report) override { emit reportWritten(report); }
    void finish(const QByteArray& report) override { final = report; emit closed(); }
    void feed(const QByteArray& text) { emit reportReceived(AntennaAnalyzerModel::commandReport(text)); }
};

class AetherLabWidgetTest final : public QObject {
    Q_OBJECT
private slots:
    void stickSelectionLimitsAndExperimentalStatus()
    {
        AntennaAnalyzerModel model;
        auto* input = new WindowInput(&model);
        model.setUsbBackend([] {
            return QList<RigExpertUsbDevice>{{"230", "423012345", "RigExpert"},
                {"500", "450112345", "RigExpert"}, {"pro", "460012345", "RigExpert"},
                {"xpro", "499812345", "RigExpert"}};
        }, [input](QObject*) { return input; });
        AetherLabWidget window(&model);
        window.resize(1200, 900);
        window.show();
        auto* devices = window.findChild<QComboBox*>("aetherLabDevice");
        auto* start = window.findChild<QDoubleSpinBox*>("aetherLabStartMHz");
        auto* stop = window.findChild<QDoubleSpinBox*>("aetherLabStopMHz");
        auto* support = window.findChild<QLabel*>("aetherLabModelSupport");
        QVERIFY(devices && start && stop && support);
        QVERIFY(support->text().contains("Stick 230, Stick 500, Stick XPro: Experimental"));
        QVERIFY(model.refreshDevices());
        QTRY_COMPARE(devices->count(), 4);
        const QList<double> maxima{230.0, 500.0, 600.0, 1000.0};
        for (int i = 0; i < devices->count(); ++i) {
            devices->setCurrentIndex(i);
            QCOMPARE(start->minimum(), 0.1);
            QCOMPARE(start->maximum(), maxima.at(i));
            QCOMPARE(stop->maximum(), maxima.at(i));
            QCOMPARE(support->text().contains("Experimental"), i != 2);
            QCOMPARE(devices->itemText(i).contains("Experimental"), i != 2);
            QVERIFY(stop->accessibleDescription().contains(QString::number(maxima.at(i))));
        }
        stop->setValue(1000.0);
        devices->setCurrentIndex(0);
        QCOMPARE(stop->value(), 230.0);
        // A bridge connection can target a different index from the preview.
        QVERIFY(model.connectDevice(3));
        input->feed("Stick XPro 123\r\n");
        AntennaAnalyzerModelTestAccess::settled(model);
        QTRY_VERIFY(support->text().contains("Stick XPro · 0.1–1000 MHz · Experimental"));
        QTRY_VERIFY(!devices->isEnabled());
        QCOMPARE(devices->currentIndex(), 3);
        QVERIFY(support->text().contains("hardware testing pending"));
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(support);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), support->text());
        // Programmatic combo changes during a session cannot change its bounds.
        devices->setCurrentIndex(0);
        QCOMPARE(devices->currentIndex(), 3);
        QCOMPARE(stop->maximum(), 1000.0);
        model.disconnectDevice();
    }
    void measuredMarkerAndClose()
    {
        AntennaAnalyzerModel model;
        auto* input = new WindowInput(&model);
        model.setUsbBackend([] { return QList<RigExpertUsbDevice>{{"injected", "460012345", "Stick Pro"}}; },
                            [input](QObject*) { return input; });
        AetherLabWidget window(&model);
        window.resize(1200, 900);
        window.show();
        auto* sweep = window.findChild<QPushButton*>("aetherLabSweep");
        auto* marker = window.findChild<QSlider*>("aetherLabMarker");
        auto* frequency = window.findChild<QLabel*>("aetherLabMarkerFrequency");
        auto* readout = window.findChild<QLabel*>("aetherLabValue_swr");
        auto* isolation = window.findChild<QCheckBox*>("aetherLabIsolated");
        QVERIFY(sweep && marker && frequency && readout && isolation);
        QVERIFY(!sweep->isEnabled());
        QVERIFY(model.refreshDevices());
        QVERIFY(model.connectDevice(0));
        input->feed("Stick Pro 1.234 REV 7\r\n");
        AntennaAnalyzerModelTestAccess::settled(model);
        QTRY_COMPARE(window.findChild<QComboBox*>("aetherLabDevice")->count(), 1);
        QVERIFY(!isolation->isChecked());
        isolation->setChecked(true);
        QVERIFY(sweep->isEnabled());
        QVERIFY(model.sweep(14000000, 14350000, 3, true));
        input->feed("OK\r\n");
        input->feed("OK\r\n");
        input->feed("14,100,0\r\n14.175,50,0\r\n14.2,-10,0\r\n14.35,54,0\r\n");
        QTRY_COMPARE(marker->maximum(), 3);
        marker->setValue(3);
        QCOMPARE(frequency->text(), QStringLiteral("14.350000 MHz"));
        QCOMPARE(readout->text(), QStringLiteral("1.080"));
        const auto accessibleText = [](QWidget* widget) {
            QAccessibleInterface* interface = QAccessible::queryAccessibleInterface(widget);
            return interface ? interface->text(QAccessible::Name) : QString();
        };
        QCOMPARE(accessibleText(frequency), QStringLiteral("Marker frequency: 14.350000 MHz"));
        QCOMPARE(accessibleText(readout), QStringLiteral("SWR: 1.080"));
        auto* status = window.findChild<QLabel*>("aetherLabStatus");
        QVERIFY(status);
        QCOMPARE(accessibleText(status), status->text());
        QVERIFY(accessibleText(status).startsWith("Complete · Stick Pro"));
        for (QLabel* value : window.findChildren<QLabel*>()) {
            if (!value->objectName().startsWith("aetherLabValue_")) { continue; }
            QVERIFY2(accessibleText(value).contains(value->text()), qPrintable(value->objectName()));
        }
        marker->setValue(1);
        QCOMPARE(readout->text(), QStringLiteral("1.000"));
        QCOMPARE(accessibleText(frequency), QStringLiteral("Marker frequency: 14.175000 MHz"));
        QCOMPARE(accessibleText(readout), QStringLiteral("SWR: 1.000"));
        QCOMPARE(accessibleText(window.findChild<QLabel*>("aetherLabValue_returnLossDb")), QStringLiteral("Return loss: ∞ dB"));
        QCOMPARE(accessibleText(window.findChild<QLabel*>("aetherLabValue_reflectionPhaseDeg")), QStringLiteral("Phase ∠S11: undefined"));
        QCOMPARE(window.findChild<QLabel*>("aetherLabValue_returnLossDb")->text(), QStringLiteral("∞ dB"));
        QCOMPARE(window.findChild<QLabel*>("aetherLabValue_reflectionPhaseDeg")->text(), QStringLiteral("undefined"));
        // A non-passive row has no return loss; the readout must not carry a unit.
        // The layout checks below keep this marker, whose readouts are the widest.
        marker->setValue(2);
        QCOMPARE(window.findChild<QLabel*>("aetherLabValue_returnLossDb")->text(), QStringLiteral("undefined"));
        QCOMPARE(accessibleText(window.findChild<QLabel*>("aetherLabValue_returnLossDb")), QStringLiteral("Return loss: undefined"));
        QVERIFY(!sweep->isEnabled());
        QWidget* plot = window.findChild<QWidget*>("aetherLabPlot");
        QVERIFY(plot);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(plot);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Value).contains("4 measured rows"));
        QVERIFY(!window.grab().isNull());
        window.findChild<QDoubleSpinBox*>("aetherLabStartMHz")->setValue(600.0);
        window.findChild<QDoubleSpinBox*>("aetherLabStopMHz")->setValue(600.0);
        window.findChild<QSpinBox*>("aetherLabIntervals")->setValue(1000);
        for (const QSize size : {QSize(900, 640), QSize(1280, 800), QSize(1440, 960), QSize(1920, 1080)}) {
            window.resize(size);
            QCoreApplication::processEvents();
            QCOMPARE(window.size(), size);
            auto* content = window.findChild<QWidget*>("aetherLabContent");
            auto* scroll = window.findChild<QScrollArea*>();
            QVERIFY(content && scroll);
            QVERIFY(content->width() <= scroll->viewport()->width());
            QVERIFY(plot->width() >= 360);
            for (const char* name : {"aetherLabStartMHz", "aetherLabStopMHz", "aetherLabIntervals"}) {
                auto* spin = window.findChild<QAbstractSpinBox*>(QLatin1String(name));
                QVERIFY(spin);
                auto* editor = spin->findChild<QLineEdit*>();
                QVERIFY(editor);
                const QMargins margins = editor->textMargins();
                const QRect textArea = editor->contentsRect().marginsRemoved(margins);
                QVERIFY(editor->geometry().left() + margins.left() >= 6);
                QVERIFY(textArea.width() >= editor->fontMetrics().horizontalAdvance(editor->text()));
                QVERIFY(textArea.height() >= editor->fontMetrics().height());
            }
            const int numberSize = frequency->font().pixelSize();
            for (const char* key : {"swr", "returnLossDb", "impedanceMagnitudeOhm", "reflectionPhaseDeg", "reflectionMagnitude", "impedancePhaseDeg"}) {
                auto* value = window.findChild<QLabel*>(QStringLiteral("aetherLabValue_") + QLatin1String(key));
                QVERIFY(value);
                QCOMPARE(value->font().pixelSize(), numberSize);
                QVERIFY(value->width() >= value->fontMetrics().horizontalAdvance(value->text()));
            }
            auto* badge = window.findChild<QLabel*>("aetherLabIsolationWarning");
            auto* back = window.findChild<QPushButton*>("aetherLabReturn");
            QVERIFY(badge && back);
            QCOMPARE(badge->text(), QStringLiteral("Radio TX is not inhibited"));
            const int badgeCenter = badge->mapTo(&window, badge->rect().center()).y();
            const int backCenter = back->mapTo(&window, back->rect().center()).y();
            QVERIFY(qAbs(badgeCenter - backCenter) <= 1);
            QVERIFY(badge->mapTo(&window, QPoint()).x() < back->mapTo(&window, QPoint()).x());
        }
        QVERIFY(window.findChild<QLabel*>("aetherLabStatus")->text().startsWith("Complete · Stick Pro"));
        QCOMPARE(window.findChild<QLabel*>("aetherLabSubtitle")->text(), QStringLiteral("Antenna Quick Check"));
        QCOMPARE(model.snapshot().value("state").toString(), QStringLiteral("complete"));
        QSignalSpy returned(&window, &AetherLabWidget::returnRequested);
        window.findChild<QPushButton*>("aetherLabReturn")->click();
        QCOMPARE(returned.count(), 1);
        window.close();
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Complete);
    }
    void minimumSwrNeedsMeasuredRow()
    {
        AntennaAnalyzerModel model;
        auto* input = new WindowInput(&model);
        model.setUsbBackend([] { return QList<RigExpertUsbDevice>{{"injected", "460012345", "Stick Pro"}}; },
                            [input](QObject*) { return input; });
        AetherLabWidget window(&model);
        window.show();
        QVERIFY(model.refreshDevices());
        QVERIFY(model.connectDevice(0));
        input->feed("Stick Pro 1.234 REV 7\r\n");
        AntennaAnalyzerModelTestAccess::settled(model);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        input->feed("OK\r\n");
        input->feed("OK\r\n");
        // Every row is non-passive, so the sweep has no SWR minimum to select.
        input->feed("14,-5,0\r\n14.175,-6,0\r\n14.35,-7,0\r\n");
        auto* marker = window.findChild<QSlider*>("aetherLabMarker");
        auto* minimum = window.findChild<QPushButton*>("aetherLabMinimum");
        QVERIFY(marker && minimum);
        QTRY_COMPARE(marker->maximum(), 2);
        QVERIFY(!minimum->isEnabled());
        marker->setValue(2);
        minimum->click();
        QCOMPARE(marker->value(), 2);
    }
};
QTEST_MAIN(AetherLabWidgetTest)
#include "aetherlab_window_test.moc"
