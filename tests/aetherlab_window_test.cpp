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
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        input->feed("OK\r\n");
        input->feed("OK\r\n");
        input->feed("14,100,0\r\n14.175,50,0\r\n14.35,54,0\r\n");
        QTRY_COMPARE(marker->maximum(), 2);
        marker->setValue(2);
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
        QVERIFY(!sweep->isEnabled());
        QWidget* plot = window.findChild<QWidget*>("aetherLabPlot");
        QVERIFY(plot);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(plot);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Value).contains("3 measured rows"));
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
};
QTEST_MAIN(AetherLabWidgetTest)
#include "aetherlab_window_test.moc"
