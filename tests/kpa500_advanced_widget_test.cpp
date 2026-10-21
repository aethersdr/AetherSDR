#include "TestSettingsProfile.h"
#include "gui/Kpa500AdvancedWidget.h"
#include "core/Kpa500Connection.h"
#include <QBuffer>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <QtTest>

namespace AetherSDR {
// An injected byte sink only; no listener, serial port or third-party peer.
class Kpa500ConnectionTest {
public:
    static void ready(Kpa500Connection& connection, QBuffer& sink)
    {
        connection.m_device = &sink;
        connection.m_connected = true;
        connection.m_responding = true;
        connection.m_powerCheckOnly = false;
        connection.m_powerOffRequested = false;
        connection.m_followTx = false;
        connection.m_sentBand = -1;
        connection.m_status.powerOn = true;
        connection.m_status.operate = false;
        connection.m_status.forwardPowerW = 0.0f;
        connection.m_status.serialNumber = QStringLiteral("09999");
        connection.m_status.firmwareVersion = QStringLiteral("01.54");
        connection.m_clock.start();
    }
};

class Kpa500AdvancedWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }

    void fileChooserReturnsWithoutNestingAndRejectsReentry()
    {
        Kpa500AdvancedWidget panel(nullptr);
        panel.show();
        bool nestedLoop = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, &panel, [&] {
            nestedLoop = true;
            for (QFileDialog* dialog : panel.findChildren<QFileDialog*>()) {
                dialog->reject();
            }
        });
        watchdog.start(100);
        panel.chooseBackup();
        watchdog.stop();
        QVERIFY2(!nestedLoop, "Opening a file chooser must return without a nested event loop");
        QPointer<QFileDialog> first = panel.findChild<QFileDialog*>();
        QVERIFY(first);
        QVERIFY(first->isVisible());
        panel.chooseBackup();
        panel.m_faultsLoaded = true;
        panel.exportFaults();
        QCOMPARE(panel.findChildren<QFileDialog*>().size(), 1);
        QCOMPARE(first->windowTitle(), QStringLiteral("Choose KPA500 configuration"));
        first->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!first);
        QVERIFY(!panel.m_backup);
        panel.chooseBackup();
        QPointer<QFileDialog> second = panel.findChild<QFileDialog*>();
        QVERIFY(second);
        second->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!second);
    }

    void fileChooserDiesWithItsParent()
    {
        auto parent = std::make_unique<QWidget>();
        auto* panel = new Kpa500AdvancedWidget(nullptr, parent.get());
        parent->show();
        panel->chooseBackup();
        QPointer<QFileDialog> dialog = panel->findChild<QFileDialog*>();
        QVERIFY(dialog);
        parent.reset();
        QVERIFY(!dialog);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void backupSelectionRechecksReadAvailability()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        Kpa500Connection connection;
        Kpa500ConnectionTest::ready(connection, sink);
        Kpa500AdvancedWidget panel(&connection);
        panel.show();
        panel.saveBackup();
        QPointer<QFileDialog> dialog = panel.findChild<QFileDialog*>();
        QVERIFY(dialog);
        dialog->selectFile(temporary.filePath("backup.KPAConfig"));
        connection.setTransmitting(true);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!dialog);
        QVERIFY(sink.data().isEmpty());
        QVERIFY(!connection.maintenanceBusy());
        QVERIFY(panel.m_status->text().contains("Backup was not started"));
        QVERIFY(!QFile::exists(temporary.filePath("backup.KPAConfig")));
    }

    void asyncFileSelectionPreviewsAndExportsWithoutAmplifierCommands()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString backupPath = temporary.filePath("backup.KPAConfig");
        QFile backup(backupPath);
        QVERIFY(backup.open(QIODevice::WriteOnly));
        const QByteArray encoded = Kpa500::encodeConfigurationBackup(
            {QByteArray(224, '\0'), QStringLiteral("01.54"), QStringLiteral("09999")},
            QDateTime::currentDateTime());
        QCOMPARE(backup.write(encoded), encoded.size());
        backup.close();
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        Kpa500Connection connection;
        Kpa500ConnectionTest::ready(connection, sink);
        Kpa500AdvancedWidget panel(&connection);
        panel.show();
        panel.chooseBackup();
        QPointer<QFileDialog> dialog = panel.findChild<QFileDialog*>();
        QVERIFY(dialog);
        dialog->selectFile(backupPath);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!dialog);
        QVERIFY(panel.m_backup);
        QVERIFY(panel.m_backupPreview->text().contains("CRC32 verified"));
        QVERIFY(!panel.m_restore->isEnabled());

        QByteArray faults(1024, char(0xff));
        faults[0xfe] = 1;
        faults[0xff] = 0;
        faults.replace(0x100, 6, QByteArray::fromHex("563412685D00"));
        panel.beginOperation(Kpa500AdvancedWidget::Operation::FaultRead);
        emit connection.faultHistoryRead(faults);
        emit connection.maintenanceFinished("Read complete");
        const QString csvPath = temporary.filePath("faults.csv");
        panel.exportFaults();
        dialog = panel.findChild<QFileDialog*>();
        QVERIFY(dialog);
        dialog->selectFile(csvPath);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!dialog);
        QFile csv(csvPath);
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const QByteArray rows = csv.readAll();
        QVERIFY(rows.startsWith("\"Slot\",\"Uptime (s)\",\"Band\""));
        QVERIFY(rows.contains("\"1193046\",\"20m\""));
        QVERIFY(rows.contains("60V HIGH"));
        QVERIFY(panel.m_status->text().contains("exported"));
        QVERIFY(sink.data().isEmpty());
    }

    void filePreviewRequiresExplicitMatchingRestore()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        Kpa500Connection connection;
        Kpa500ConnectionTest::ready(connection, sink);
        Kpa500AdvancedWidget panel(&connection);
        QSignalSpy busy(&connection, &Kpa500Connection::maintenanceBusyChanged);
        const QString path = temporary.filePath("test.KPAConfig");
        const auto writeBackup = [&path](const QString& serial, const QString& firmware) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) { return false; }
            const QByteArray encoded = Kpa500::encodeConfigurationBackup(
                {QByteArray(224, '\0'), firmware, serial}, QDateTime::currentDateTime());
            return !encoded.isEmpty() && file.write(encoded) == encoded.size();
        };
        QVERIFY(writeBackup("09999", "01.54"));
        QVERIFY(panel.loadBackup(path));
        QVERIFY(panel.m_backupPreview->text().contains("CRC32 verified"));
        QVERIFY(panel.m_confirmRestore->isEnabled());
        QVERIFY(!panel.m_restore->isEnabled());
        QVERIFY(sink.data().isEmpty());
        QCOMPARE(busy.count(), 0);
        panel.m_confirmRestore->setChecked(true);
        QVERIFY(panel.m_restore->isEnabled());
        QVERIFY(sink.data().isEmpty());
        panel.m_restore->click();
        QVERIFY(connection.maintenanceBusy());
        QVERIFY(!sink.data().contains("^EW"));
        QVERIFY(!panel.m_readBanner->isEnabled());
        QVERIFY(!panel.m_chooseBackup->isEnabled());
        panel.m_cancel->click();
        QVERIFY(!connection.maintenanceBusy());
        QVERIFY(!panel.m_confirmRestore->isChecked());

        Kpa500ConnectionTest::ready(connection, sink);
        QVERIFY(writeBackup("08888", "01.54"));
        QVERIFY(panel.loadBackup(path));
        QVERIFY(!panel.m_confirmRestore->isEnabled());
        QVERIFY(!panel.m_restore->isEnabled());
        QVERIFY(panel.m_restore->accessibleDescription().contains("serial number"));
        QVERIFY(writeBackup("09999", "01.53"));
        QVERIFY(panel.loadBackup(path));
        QVERIFY(!panel.m_restore->isEnabled());
        QFile invalid(path);
        QVERIFY(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate));
        invalid.write("not a backup");
        invalid.close();
        QVERIFY(!panel.loadBackup(path));
        QVERIFY(!panel.m_backup);
        QVERIFY(!panel.m_restore->isEnabled());
    }

    void bannerAndConnectionStateNeverPretendReadback()
    {
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        Kpa500Connection connection;
        Kpa500ConnectionTest::ready(connection, sink);
        Kpa500AdvancedWidget panel(&connection);
        QVERIFY(!panel.m_writeBanner->isEnabled());
        panel.beginOperation(Kpa500AdvancedWidget::Operation::BannerRead);
        emit connection.powerOnBannerRead("OLD");
        emit connection.maintenanceFinished("Read complete");
        QCOMPARE(panel.m_banner->text(), QStringLiteral("OLD"));
        QVERIFY(!panel.m_writeBanner->isEnabled());
        panel.m_banner->setText(QString::fromUtf8("café"));
        QVERIFY(!panel.m_writeBanner->isEnabled());
        panel.m_banner->setText("NEW");
        QVERIFY(panel.m_writeBanner->isEnabled());
        QVERIFY(panel.m_bannerReadback->text().contains("OLD"));
        QVERIFY(sink.data().isEmpty());
        connection.setTransmitting(true);
        QVERIFY(!panel.m_writeBanner->isEnabled());
        QVERIFY(!panel.m_readBanner->isEnabled());
        connection.setTransmitting(false);
        emit connection.disconnected();
        QVERIFY(!panel.m_bannerLoaded);
        QVERIFY(!panel.m_writeBanner->isEnabled());
        QVERIFY(panel.m_banner->text().isEmpty());
    }

    void backupSaveFailureAndIdentityChangeAreVisible()
    {
        QTemporaryDir temporary;
        QBuffer sink;
        sink.open(QIODevice::ReadWrite);
        Kpa500Connection connection;
        Kpa500ConnectionTest::ready(connection, sink);
        Kpa500AdvancedWidget panel(&connection);
        panel.m_savePath = temporary.filePath("backup.KPAConfig");
        panel.beginOperation(Kpa500AdvancedWidget::Operation::Backup);
        emit connection.configurationRead(QByteArray(224, '\0'));
        emit connection.maintenanceFinished("Read complete");
        QFile file(panel.m_savePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(Kpa500::decodeConfigurationBackup(file.readAll()));
        QVERIFY(panel.m_status->text().contains("Backup saved"));
        panel.m_savePath = temporary.filePath("must-not-exist.KPAConfig");
        panel.beginOperation(Kpa500AdvancedWidget::Operation::Backup);
        panel.m_operationSerial = "01234";
        emit connection.configurationRead(QByteArray(224, '\0'));
        emit connection.maintenanceFinished("Read complete");
        QVERIFY(!QFile::exists(panel.m_savePath));
        QVERIFY(panel.m_status->text().contains("identity changed"));
        panel.m_savePath = temporary.filePath("missing/directory/backup.KPAConfig");
        panel.beginOperation(Kpa500AdvancedWidget::Operation::Backup);
        emit connection.configurationRead(QByteArray(224, '\0'));
        emit connection.maintenanceFinished("Read complete");
        QVERIFY(panel.m_status->text().contains("not saved"));
    }
};
} // namespace AetherSDR
int main(int argc, char* argv[])
{
    // File pickers must not enumerate the user's Documents folder in headless tests.
    TestSettingsProfile profile(QStringLiteral("kpa500-advanced-widget-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AetherSDR::Kpa500AdvancedWidgetTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "kpa500_advanced_widget_test.moc"
