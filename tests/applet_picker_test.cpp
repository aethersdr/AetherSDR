#include "gui/AppletPicker.h"
#include "core/ThemeManager.h"
#include <QComboBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QAbstractItemView>
#include <QPainter>
#include <QtTest>

using namespace AetherSDR;

class AppletPickerTest : public QObject {
    Q_OBJECT

private slots:
    void selectionRequiresExplicitAdd()
    {
        AppletPicker picker;
        picker.setEntries({{"RX", "RX Controls", "Receive"},
                           {"VU", "S-Meter", "Metering", true, true},
                           {"AMP", "PGXL", "Transmit", false, false}});
        auto* combo = picker.findChild<QComboBox*>("appletPickerCombo");
        auto* add = picker.findChild<QPushButton*>("addAppletButton");
        QSignalSpy requested(&picker, &AppletPicker::addRequested);
        QVERIFY(combo);
        QVERIFY(add);
        QVERIFY(!add->isEnabled());
        QCOMPARE(combo->currentIndex(), -1);
        combo->setCurrentIndex(combo->findData("RX"));
        QCOMPARE(requested.count(), 0);
        QVERIFY(add->isEnabled());
        QTest::keyClick(add, Qt::Key_Space);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toString(), QStringLiteral("RX"));
        for (const QString& id : {QStringLiteral("VU"), QStringLiteral("AMP")}) {
            combo->setCurrentIndex(combo->findData(id));
            QVERIFY(!add->isEnabled());
            add->click();
        }
        QCOMPARE(requested.count(), 1);
    }

    void groupingAndLiveAvailability()
    {
        AppletPicker picker;
        QList<AppletPicker::Entry> entries = {
            {"RX", "RX Controls", "Receive"}, {"VU", "S-Meter", "Metering"},
            {"PWR", "Power & SWR", "Metering"}, {"AMP", "PGXL", "Amplifiers", false}};
        picker.setEntries(entries);
        auto* combo = picker.findChild<QComboBox*>("appletPickerCombo");
        auto* add = picker.findChild<QPushButton*>("addAppletButton");
        auto* model = qobject_cast<QStandardItemModel*>(combo->model());
        QCOMPARE(combo->count(), 7);
        QVERIFY(!model->item(0)->isEnabled());
        QCOMPARE(combo->itemText(0), QStringLiteral("Amplifiers"));
        QCOMPARE(combo->findData("VU"), combo->findData("PWR") + 1);
        combo->setCurrentIndex(combo->findData("RX"));
        entries[3].available = true;
        picker.setEntries(entries);
        QCOMPARE(combo->currentData().toString(), QStringLiteral("RX"));
        QVERIFY(model->item(combo->findData("AMP"))->isEnabled());
        entries[0].open = true;
        picker.setEntries(entries);
        QCOMPARE(combo->currentIndex(), -1);
        QVERIFY(!add->isEnabled());
        entries[0].open = false;
        picker.setEntries(entries);
        QVERIFY(model->item(combo->findData("RX"))->isEnabled());
        entries[0].available = false;
        combo->setCurrentIndex(combo->findData("RX"));
        picker.setEntries(entries);
        QVERIFY(!add->isEnabled());
    }

    void themesAndCompactLayout()
    {
        AppletPicker picker;
        picker.resize(260, 36);
        picker.show();
        picker.setEntries({{"RX", "RX Controls", "Receive"}});
        auto* combo = picker.findChild<QComboBox*>("appletPickerCombo");
        auto* add = picker.findChild<QPushButton*>("addAppletButton");
        for (const QString& theme : {QStringLiteral("Default Light"), QStringLiteral("Default Dark")}) {
            QVERIFY(ThemeManager::instance().setActiveTheme(theme));
            QCoreApplication::processEvents();
            QVERIFY(!picker.styleSheet().contains("{{"));
            QVERIFY(!combo->styleSheet().contains("{{"));
            QVERIFY(picker.rect().contains(add->geometry()));
            QVERIFY(combo->width() > 100);
            QVERIFY(!combo->accessibleName().isEmpty());
            QVERIFY(!add->accessibleName().isEmpty());
            QImage header(240, 26, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&header);
            QStyleOptionViewItem option;
            option.initFrom(combo->view());
            option.widget = combo->view();
            option.rect = header.rect();
            combo->itemDelegate()->paint(&painter, option, combo->model()->index(0, 0));
            painter.end();
            QCOMPARE(header.pixelColor(0, 0),
                     ThemeManager::instance().color(combo->view(), "color.background.2"));
            QVERIFY(!combo->model()->index(0, 0).flags().testFlag(Qt::ItemIsEnabled));
        }
    }
};

QTEST_MAIN(AppletPickerTest)
#include "applet_picker_test.moc"
