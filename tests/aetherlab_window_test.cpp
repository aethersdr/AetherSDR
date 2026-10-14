#include "gui/AetherLabWindow.h"
#include "models/AntennaAnalyzerModel.h"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
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

class AetherLabWindowTest final : public QObject {
    Q_OBJECT
private slots:
    void measuredMarkerAndClose()
    {
        AntennaAnalyzerModel model;
        auto* input = new WindowInput(&model);
        model.setUsbBackend([] { return QList<RigExpertUsbDevice>{{"injected", "460012345", "Stick Pro"}}; },
                            [input](QObject*) { return input; });
        AetherLabWindow window(&model);
        window.show();
        auto* sweep = window.findChild<QPushButton*>("aetherLabSweep");
        auto* marker = window.findChild<QSlider*>("aetherLabMarker");
        auto* readout = window.findChild<QLabel*>("aetherLabReadout");
        auto* isolation = window.findChild<QCheckBox*>("aetherLabIsolated");
        QVERIFY(sweep && marker && readout && isolation);
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
        QVERIFY(readout->text().contains("14.350000 MHz"));
        QVERIFY(readout->text().contains("SWR  1.080"));
        marker->setValue(1);
        QVERIFY(readout->text().contains("SWR  1.000"));
        QVERIFY(readout->text().contains("RL  ∞"));
        QVERIFY(!sweep->isEnabled());
        QWidget* plot = window.findChild<QWidget*>("aetherLabPlot");
        QVERIFY(plot);
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(plot);
        QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Value).contains("3 measured rows"));
        QVERIFY(!window.grab().isNull());
        window.close();
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Complete);
    }
};
QTEST_MAIN(AetherLabWindowTest)
#include "aetherlab_window_test.moc"
