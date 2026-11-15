#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/CatPort.h"
#include "gui/CatControlApplet.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QTest>

using namespace AetherSDR;

class OtrspAppletTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        AppSettings::instance().load();
    }

    void validatesSavedMappingsAndReportsFailures()
    {
        AppSettings& settings = AppSettings::instance();
        settings.setValue("CatEnabled", "False");
        settings.setValue("CatPort_0_Port", "");
        settings.setValue("CatPort_0_Dialect", "OTRSP");
        settings.setValue("CatPort_0_VfoA", "2");
        settings.setValue("CatPort_0_VfoB", "1");
        settings.setValue("CatPort_0_Enabled", "False");
        settings.save();
        CatPort port(nullptr);
        CatPort second(nullptr);
        CatPort* ports[] = {&port, &second};
        CatControlApplet applet;
        applet.setPorts(ports, 2);
        applet.setMaxSlices(2);
        applet.setFloating(true);
        auto* enabled = applet.findChild<QCheckBox*>("catPort0Enabled");
        auto* first = applet.findChild<QComboBox*>("catPort0VfoA");
        auto* other = applet.findChild<QComboBox*>("catPort0VfoB");
        auto* number = applet.findChild<QLineEdit*>("catPort0Port");
        auto* status = applet.findChild<QLabel*>("catPort0Status");
        QVERIFY(enabled && first && other && number && status);
        QCOMPARE(number->text(), QStringLiteral("60000"));
        QCOMPARE(first->currentData().toInt(), 0);
        QCOMPARE(other->currentData().toInt(), 1);
        QVERIFY(!enabled->isEnabled());
        QVERIFY(enabled->accessibleDescription().contains("available"));
        QCOMPARE(settings.value("CatPort_0_VfoA").toInt(), 2);
        applet.setMaxSlices(8);
        QVERIFY(enabled->isEnabled());
        enabled->setChecked(true);
        QVERIFY(first->isEnabled()); // master is off; configuration remains editable
        other->setCurrentIndex(other->findData(CatPort::kVfoNone));
        QCOMPARE(status->text(), QStringLiteral("Error"));
        QVERIFY(status->accessibleDescription().contains("two distinct"));
        QVERIFY(enabled->isEnabled());
        enabled->setChecked(false);
        other->setCurrentIndex(other->findData(1));

        port.setDialect(CatDialect::OTRSP);
        port.setVfoA(0);
        port.setVfoB(0);
        QVERIFY(!port.start(0)); // rejected before opening any socket
        QCOMPARE(status->text(), QStringLiteral("Error"));
        QCOMPARE(status->accessibleDescription(), port.errorString());
        port.stop();
        QVERIFY(status->accessibleDescription().isEmpty());
        for (const QString& suffix : {QStringLiteral("Enabled"), QStringLiteral("Port"),
                                     QStringLiteral("Dialect"), QStringLiteral("VfoA"),
                                     QStringLiteral("VfoB")}) {
            QWidget* a = applet.findChild<QWidget*>("catPort0" + suffix);
            QWidget* b = applet.findChild<QWidget*>("catPort1" + suffix);
            QVERIFY(a && b);
            QVERIFY(a->accessibleName().contains("port A"));
            QVERIFY(b->accessibleName().contains("port B"));
            QVERIFY(a->accessibleName() != b->accessibleName());
        }
    }

    void restoresMappingsAfterReceiverCountChanges()
    {
        AppSettings& settings = AppSettings::instance();
        settings.setValue("CatEnabled", "True");
        settings.setValue("CatPort_0_Port", "60000");
        settings.setValue("CatPort_0_Dialect", "OTRSP");
        settings.setValue("CatPort_0_VfoA", "2");
        settings.setValue("CatPort_0_VfoB", "5");
        settings.setValue("CatPort_0_Enabled", "False");
        settings.save();
        settings.load();
        CatPort port(nullptr);
        CatPort* ports[] = {&port};
        CatControlApplet applet;
        applet.setPorts(ports, 1);
        applet.setMaxSlices(1);
        applet.setFloating(true);
        QComboBox* vfoA = applet.findChild<QComboBox*>("catPort0VfoA");
        QComboBox* vfoB = applet.findChild<QComboBox*>("catPort0VfoB");
        QComboBox* dialect = applet.findChild<QComboBox*>("catPort0Dialect");
        QLineEdit* number = applet.findChild<QLineEdit*>("catPort0Port");
        QCheckBox* enabled = applet.findChild<QCheckBox*>("catPort0Enabled");
        QVERIFY(vfoA && vfoB && dialect && number && enabled);
        QCOMPARE(vfoA->currentData().toInt(), 0);
        QCOMPARE(vfoB->currentData().toInt(), CatPort::kVfoNone);
        QVERIFY(!enabled->isEnabled());
        applet.setMaxSlices(8);
        QCOMPARE(vfoA->currentData().toInt(), 2);
        QCOMPARE(vfoB->currentData().toInt(), 5);
        QVERIFY(enabled->isEnabled());
        applet.setMaxSlices(1);
        number->setText("60002");
        QVERIFY(QMetaObject::invokeMethod(number, "editingFinished", Qt::DirectConnection));
        QCOMPARE(settings.value("CatPort_0_VfoA").toInt(), 2);
        QCOMPARE(settings.value("CatPort_0_VfoB").toInt(), 5);
        applet.setMaxSlices(8);
        QCOMPARE(vfoA->currentData().toInt(), 2);
        QCOMPARE(vfoB->currentData().toInt(), 5);
        vfoB->setCurrentIndex(vfoB->findData(1));
        applet.setMaxSlices(2);
        QCOMPARE(vfoB->currentData().toInt(), 1);
        applet.setMaxSlices(8);
        QCOMPARE(vfoB->currentData().toInt(), 1);
        dialect->setCurrentIndex(dialect->findData(static_cast<int>(CatDialect::Rigctld)));
        applet.setMaxSlices(2);
        QCOMPARE(vfoB->currentData().toInt(), CatPort::kVfoNone);
        QVERIFY(!vfoB->isEnabled());
        applet.setMaxSlices(8);
        QCOMPARE(vfoB->currentData().toInt(), CatPort::kVfoNone);
        dialect->setCurrentIndex(dialect->findData(static_cast<int>(CatDialect::OTRSP)));
        QCOMPARE(vfoB->currentData().toInt(), 1);
    }

    void settingsAndMappings()
    {
        AppSettings& settings = AppSettings::instance();
        settings.setValue("CatEnabled", "True");
        settings.setValue("CatPort_0_Port", "");
        settings.setValue("CatPort_0_Dialect", "FlexCAT");
        settings.setValue("CatPort_0_VfoA", "2");
        settings.setValue("CatPort_0_VfoB", "-1");
        settings.setValue("CatPort_0_Enabled", "False");
        settings.save();
        CatPort port(nullptr);
        CatPort* ports[] = {&port};
        {
            CatControlApplet applet;
            applet.setPorts(ports, 1);
            applet.setMaxSlices(8);
            applet.setFloating(true);
            QComboBox* dialect = applet.findChild<QComboBox*>("catPort0Dialect");
            QComboBox* vfoA = applet.findChild<QComboBox*>("catPort0VfoA");
            QComboBox* vfoB = applet.findChild<QComboBox*>("catPort0VfoB");
            QLineEdit* number = applet.findChild<QLineEdit*>("catPort0Port");
            QCheckBox* enabled = applet.findChild<QCheckBox*>("catPort0Enabled");
            QVERIFY(dialect && vfoA && vfoB && number && enabled);
            dialect->setCurrentIndex(dialect->findData(static_cast<int>(CatDialect::OTRSP)));
            QCOMPARE(number->text(), QStringLiteral("60000"));
            QCOMPARE(settings.value("CatPort_0_Dialect").toString(), QStringLiteral("OTRSP"));
            QCOMPARE(settings.value("CatPort_0_Port").toInt(), 60000);
            QVERIFY(vfoB->isEnabled());
            QVERIFY(!enabled->isEnabled());
            QVERIFY(enabled->accessibleDescription().contains("two distinct"));
            vfoB->setCurrentIndex(vfoB->findData(2));
            QVERIFY(!enabled->isEnabled());
            vfoB->setCurrentIndex(vfoB->findData(5));
            QVERIFY(enabled->isEnabled());
            QCOMPARE(vfoA->accessibleName(), QStringLiteral("CAT port A: OTRSP Radio 1 slice"));
            QCOMPARE(vfoB->accessibleName(), QStringLiteral("CAT port A: OTRSP Radio 2 slice"));
            number->setText("60001");
            QVERIFY(QMetaObject::invokeMethod(number, "editingFinished", Qt::DirectConnection));
            QCOMPARE(settings.value("CatPort_0_Port").toInt(), 60001);
            enabled->setChecked(true);
            QVERIFY(number->isReadOnly());
            QVERIFY(!dialect->isEnabled());
            QVERIFY(!vfoA->isEnabled());
            QVERIFY(!vfoB->isEnabled());
            QVERIFY(vfoA->accessibleDescription().contains("Disable this CAT port"));
            QVERIFY(vfoB->accessibleDescription().contains("Disable this CAT port"));
            enabled->setChecked(false);
            dialect->setCurrentIndex(dialect->findData(static_cast<int>(CatDialect::Rigctld)));
            QVERIFY(!vfoB->isEnabled());
            QVERIFY(vfoB->accessibleDescription().contains("Rigctld uses only VFO A"));
            QCOMPARE(settings.value("CatPort_0_VfoB").toInt(), 5);
            dialect->setCurrentIndex(dialect->findData(static_cast<int>(CatDialect::OTRSP)));
            QCOMPARE(vfoB->currentData().toInt(), 5);
            QCOMPARE(number->text(), QStringLiteral("60001"));
        }
        CatControlApplet restored;
        restored.setPorts(ports, 1);
        restored.setMaxSlices(8);
        restored.setFloating(true);
        QCOMPARE(restored.findChild<QComboBox*>("catPort0Dialect")->currentData().toInt(),
                 static_cast<int>(CatDialect::OTRSP));
        QCOMPARE(restored.findChild<QLineEdit*>("catPort0Port")->text(), QStringLiteral("60001"));
        QCOMPARE(restored.findChild<QComboBox*>("catPort0VfoA")->currentData().toInt(), 2);
        QCOMPARE(restored.findChild<QComboBox*>("catPort0VfoB")->currentData().toInt(), 5);
        QVERIFY(!port.isRunning());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("otrsp-applet"));
    QApplication app(argc, argv);
    if (!profile.isValid()) {
        return 2;
    }
    OtrspAppletTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "otrsp_applet_test.moc"
