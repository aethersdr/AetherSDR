#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "gui/RadioSetupDialog.h"
#include "gui/TxBandDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <QSignalSpy>
#include <QTabWidget>
#include <QtTest>

using namespace AetherSDR;
namespace {
QLineEdit* editNextTo(QWidget& root, const QString& caption)
{
    for (QGridLayout* grid : root.findChildren<QGridLayout*>()) {
        for (int index = 0; index < grid->count(); ++index) {
            QLabel* label = qobject_cast<QLabel*>(grid->itemAt(index)->widget());
            if (!label || label->text() != caption) { continue; }
            int row, column, rows, columns;
            grid->getItemPosition(index, &row, &column, &rows, &columns);
            QLayoutItem* item = grid->itemAtPosition(row, column + 1);
            return item ? qobject_cast<QLineEdit*>(item->widget()) : nullptr;
        }
    }
    return nullptr;
}
QPushButton* buttonNamed(QWidget& root, const QString& text)
{
    for (QPushButton* button : root.findChildren<QPushButton*>()) {
        if (button->text() == text) { return button; }
    }
    return nullptr;
}
QGroupBox* groupNamed(QWidget& root, const QString& title)
{
    for (QGroupBox* group : root.findChildren<QGroupBox*>()) {
        if (group->title() == title) { return group; }
    }
    return nullptr;
}
}

class RadioSetupOperationsTest : public QObject {
    Q_OBJECT
private slots:
    void invalidCallsignEditPreservesSavedIdentity()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        radio.setStationCallsign(QStringLiteral("N0CALL"));
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Radio"));
        QLineEdit* callsign = nullptr;
        for (QLineEdit* edit : dialog.findChildren<QLineEdit*>()) {
            if (edit->accessibleName() == QStringLiteral("Station callsign")) {
                callsign = edit;
                break;
            }
        }
        QVERIFY(callsign);
        QSignalSpy dropped(&radio, &RadioModel::commandDropped);
        for (const QString& input : {QStringLiteral("#"), QStringLiteral("@_!"),
                                    QString::fromUtf8("雪"), QStringLiteral(" ")}) {
            backend->commands.clear();
            callsign->setText(input);
            emit callsign->editingFinished();
            QCOMPARE(callsign->text(), QStringLiteral("N0CALL"));
            QVERIFY(backend->commands.isEmpty());
            QString persisted;
            QVERIFY(AppSettings::instance().readAppRowFromDisk(QStringLiteral("StationCallsign"), persisted));
            QCOMPARE(persisted, QStringLiteral("N0CALL"));
        }
        QVERIFY(dropped.isEmpty());
        callsign->setText(QStringLiteral("ki4ttz/p"));
        emit callsign->editingFinished();
        QCOMPARE(backend->commands, QStringList{QStringLiteral("radio callsign KI4TTZ/P")});
        QCOMPARE(callsign->text(), QStringLiteral("KI4TTZ/P"));
        QString persisted;
        QVERIFY(AppSettings::instance().readAppRowFromDisk(QStringLiteral("StationCallsign"), persisted));
        QCOMPARE(persisted, QStringLiteral("KI4TTZ/P"));
    }

    void calibrationSeparatesAcceptanceFromCompletion()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Receive"));
        QGroupBox* group = groupNamed(dialog, QStringLiteral("Frequency Offset"));
        QVERIFY(group);
        QLineEdit* frequency = editNextTo(*group, QStringLiteral("Cal Frequency (MHz):"));
        QPushButton* start = buttonNamed(*group, QStringLiteral("Start"));
        QVERIFY(frequency && start);
        frequency->setText(QStringLiteral("10.000000"));
        start->click();
        QCOMPARE(backend->commands, QStringList{"radio set cal_freq=10.000000"});
        backend->acceptCalibrationPrelude();
        QCOMPARE(backend->commands, QStringList({"radio set cal_freq=10.000000",
                                                "radio set freq_error_ppb=0", "radio pll_start"}));
        QVERIFY(!start->isEnabled());
        backend->replyLast({RadioSetupReply::Kind::Accepted});
        QVERIFY(!start->isEnabled());
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("1")}});
        QVERIFY(!start->isEnabled());
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("0")}});
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("1")},
                                                            {QStringLiteral("freq_error_ppb"), QStringLiteral("-23")}});
        QVERIFY(start->isEnabled());
        bool completed = false;
        for (QLabel* label : group->findChildren<QLabel*>()) {
            completed |= label->text() == QStringLiteral("Complete (-23 ppb)");
        }
        QVERIFY(completed);
        backend->commands.clear();
        frequency->setText(QStringLiteral("not-a-frequency"));
        start->click();
        QVERIFY(backend->commands.isEmpty());
        QVERIFY(start->isEnabled());
    }

    void calibrationFailureDisconnectAndDialogClose()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        auto dialog = std::make_unique<RadioSetupDialog>(&radio);
        dialog->selectTab(QStringLiteral("Receive"));
        QGroupBox* group = groupNamed(*dialog, QStringLiteral("Frequency Offset"));
        QVERIFY(group);
        QLineEdit* frequency = editNextTo(*group, QStringLiteral("Cal Frequency (MHz):"));
        QPushButton* start = buttonNamed(*group, QStringLiteral("Start"));
        QVERIFY(frequency && start);
        frequency->setText(QStringLiteral("10"));
        start->click();
        backend->replyLast({RadioSetupReply::Kind::Failed, 9, QStringLiteral("rejected")});
        QVERIFY(start->isEnabled());
        QCOMPARE(backend->commands.size(), 1);
        backend->commands.clear();
        start->click();
        backend->replyLast({RadioSetupReply::Kind::Accepted});
        backend->replyLast({RadioSetupReply::Kind::Failed, 10, QStringLiteral("reset rejected")});
        QCOMPARE(backend->commands.size(), 2);
        QVERIFY(start->isEnabled());
        start->click();
        backend->acceptCalibrationPrelude();
        backend->replyLast({RadioSetupReply::Kind::Accepted});
        backend->live = false;
        emit radio.connectionStateChanged(false);
        QVERIFY(start->isEnabled());
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("0")}});
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("1")}});
        bool disconnected = false;
        for (QLabel* label : group->findChildren<QLabel*>()) {
            disconnected |= label->text() == QStringLiteral("Disconnected");
        }
        QVERIFY(disconnected);
        backend->live = true;
        emit radio.connectionStateChanged(true);
        start->click();
        const RadioSetupCompletion closed = backend->replies.back();
        const qsizetype dispatched = backend->commands.size();
        dialog.reset();
        closed({RadioSetupReply::Kind::Accepted});
        QCOMPARE(backend->commands.size(), dispatched);
        QCoreApplication::processEvents();
    }

    void calibrationCompletionDeadlineStartsAfterAcceptance()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Receive"));
        QGroupBox* group = groupNamed(dialog, QStringLiteral("Frequency Offset"));
        QVERIFY(group);
        QLineEdit* frequency = editNextTo(*group, QStringLiteral("Cal Frequency (MHz):"));
        QPushButton* start = buttonNamed(*group, QStringLiteral("Start"));
        QVERIFY(frequency && start);
        frequency->setText(QStringLiteral("10"));
        start->click();
        QTest::qWait(21000);
        QVERIFY(!start->isEnabled());
        QCOMPARE(backend->commands, QStringList{"radio set cal_freq=10.000000"});
        backend->acceptCalibrationPrelude();
        backend->replyLast({RadioSetupReply::Kind::Accepted});
        QVERIFY(!start->isEnabled());
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("0")}});
        emit radio.statusReceived(QStringLiteral("radio"), {{QStringLiteral("pll_done"), QStringLiteral("1")}});
        QVERIFY(start->isEnabled());
    }

    void transverterCreationIsLifetimeAndSessionFenced()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        auto dialog = std::make_unique<RadioSetupDialog>(&radio);
        dialog->selectTab(QStringLiteral("Transverters"));
        QPushButton* create = buttonNamed(*dialog, QStringLiteral("Create New Transverter"));
        QVERIFY(create);
        create->click();
        QCOMPARE(backend->commands, QStringList{"xvtr create"});
        const RadioSetupCompletion response = backend->replies.back();
        RadioSetupControlTestAccess::transverter(radio, 2);
        response({RadioSetupReply::Kind::Accepted});
        backend = installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::transverter(radio, 3);
        QTest::qWait(350);
        QVERIFY(!editNextTo(*dialog, QStringLiteral("RF Freq (MHz):")));
        QVERIFY(backend->commands.isEmpty());
        create->click();
        const RadioSetupCompletion closed = backend->replies.back();
        dialog.reset();
        closed({RadioSetupReply::Kind::Accepted});
        QTest::qWait(350);
    }

    void transverterNameUsesTheRadioLimit()
    {
        RadioModel radio;
        installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::transverter(radio, 2);
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Transverters"));
        QLineEdit* name = editNextTo(dialog, QStringLiteral("Name:"));
        QVERIFY(name);
        QCOMPARE(name->maxLength(), 4);
        name->setText(QStringLiteral("abcdefgh"));
        QCOMPARE(name->text(), QStringLiteral("abcd"));
    }

    void bandDialogUsesTypedRequestsAndNormalizesNumbers()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::band(radio, 9);
        radio.handleStatusForTest(QStringLiteral("transmit band 9"),
            {{QStringLiteral("hwalc_enabled"), QStringLiteral("1")}});
        TxBandDialog dialog(&radio);
        dialog.show();
        // A confirmed-on value is shown without being written back.
        QVERIFY(backend->commands.isEmpty());
        QVERIFY(dialog.findChildren<QCheckBox*>().at(7)->isChecked());
        const QList<QLineEdit*> editors = dialog.findChildren<QLineEdit*>();
        QCOMPARE(editors.size(), 2);
        editors[0]->setText(QStringLiteral("42"));
        emit editors[0]->editingFinished();
        editors[1]->setText(QStringLiteral("11"));
        emit editors[1]->editingFinished();
        QCOMPARE(backend->commands, QStringList({"transmit bandset 9 rfpower=42", "transmit bandset 9 tunepower=11"}));
        backend->commands.clear();
        editors[0]->setText(QStringLiteral("42 inhibit=0"));
        emit editors[0]->editingFinished();
        QCOMPARE(editors[0]->text(), QStringLiteral("37"));
        QVERIFY(backend->commands.isEmpty());
        editors[1]->setText(QStringLiteral("101"));
        emit editors[1]->editingFinished();
        QCOMPARE(editors[1]->text(), QStringLiteral("100"));
        QCOMPARE(backend->commands, QStringList{"transmit bandset 9 tunepower=100"});
        backend->commands.clear();
        editors[0]->setText(QStringLiteral("999999999999999999999"));
        emit editors[0]->editingFinished();
        QCOMPARE(editors[0]->text(), QStringLiteral("37"));
        QVERIFY(backend->commands.isEmpty());
        // Eight bandset toggles, then the row's RCA-suppression toggle (#6524).
        const QList<QCheckBox*> toggles = dialog.findChildren<QCheckBox*>();
        QCOMPARE(toggles.size(), 9);
        toggles[0]->setChecked(true);
        QCOMPARE(backend->commands, QStringList{"transmit bandset 9 inhibit=1"});
        // A report that doesn't carry the clicked value leaves the click in
        // place until the radio echoes it; the radio's own later report moves
        // the box without writing it back.
        backend->commands.clear();
        radio.handleStatusForTest(QStringLiteral("transmit band 9"),
            {{QStringLiteral("rfpower"), QStringLiteral("37")}});
        QCoreApplication::processEvents();
        QVERIFY(toggles[0]->isChecked());
        radio.handleStatusForTest(QStringLiteral("transmit band 9"),
            {{QStringLiteral("inhibit"), QStringLiteral("1")}});
        QCoreApplication::processEvents();
        QVERIFY(toggles[0]->isChecked());
        radio.handleStatusForTest(QStringLiteral("transmit band 9"),
            {{QStringLiteral("inhibit"), QStringLiteral("0")}});
        QCoreApplication::processEvents();
        QVERIFY(!toggles[0]->isChecked());
        QVERIFY(backend->commands.isEmpty());
        // A change the radio refuses after it was sent shows the radio's value.
        toggles[1]->setChecked(true);
        QCOMPARE(backend->commands, QStringList{"interlock bandset 9 acc_tx_enabled=1"});
        backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("refused")});
        QVERIFY(!toggles[1]->isChecked());
        toggles[2]->setChecked(true);
        backend->replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
        QVERIFY(toggles[2]->isChecked());
        editors[0]->setText(QStringLiteral("50"));
        emit editors[0]->editingFinished();
        backend->replyLast({RadioSetupReply::Kind::Failed, 0x50000015, QStringLiteral("refused")});
        QCOMPARE(editors[0]->text(), QStringLiteral("37"));
        backend->commands.clear();
        backend = installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::band(radio, 9);
        QSignalSpy dropped(&radio, &RadioModel::commandDropped);
        const QPointer<QLineEdit> stale = editors[0];
        editors[0]->setText(QStringLiteral("55"));
        emit editors[0]->editingFinished();
        QVERIFY(backend->commands.isEmpty());
        QVERIFY(dropped.isEmpty());
        // The open table is rebuilt for the new session, and its fields write.
        QCoreApplication::processEvents();
        QVERIFY(!stale);
        const QList<QLineEdit*> fresh = dialog.findChildren<QLineEdit*>();
        QCOMPARE(fresh.size(), 2);
        fresh[0]->setText(QStringLiteral("55"));
        emit fresh[0]->editingFinished();
        QCOMPARE(backend->commands, QStringList{"transmit bandset 9 rfpower=55"});
        // Text being typed when the table is rebuilt is dropped, not written.
        backend->commands.clear();
        const QList<QLineEdit*> typing = dialog.findChildren<QLineEdit*>();
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dialog));
        typing[0]->setFocus();
        QTRY_VERIFY(typing[0]->hasFocus());
        QTest::keyClicks(typing[0], QStringLiteral("9"));
        radio.transmitModel().setActiveProfile(QStringLiteral("Contest"));
        QCoreApplication::processEvents();
        QVERIFY(backend->commands.isEmpty());
        // Focus returns to the same field in the new table.
        QTRY_VERIFY(dialog.findChildren<QLineEdit*>().value(0)
                    && dialog.findChildren<QLineEdit*>().value(0)->hasFocus());
    }

    void staticNetworkExplainsInvalidFieldsWithoutDispatch()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Network"));
        QGroupBox* group = groupNamed(dialog, QStringLiteral("IP Configuration"));
        QVERIFY(group);
        QLineEdit* address = editNextTo(*group, QStringLiteral("IP Address:"));
        QLineEdit* mask = editNextTo(*group, QStringLiteral("Mask:"));
        QLineEdit* gateway = editNextTo(*group, QStringLiteral("Gateway:"));
        QPushButton* apply = buttonNamed(*group, QStringLiteral("Apply"));
        QPushButton* staticMode = buttonNamed(*group, QStringLiteral("Static"));
        QVERIFY(address && mask && gateway && apply && staticMode);
        QSignalSpy dropped(&radio, &RadioModel::commandDropped);
        staticMode->click();
        address->setText(QStringLiteral("192.0."));
        gateway->setText(QStringLiteral("192.0.2.1"));
        mask->setText(QStringLiteral("255.0.255.0"));
        QVERIFY(!apply->isEnabled());
        QVERIFY(!address->toolTip().isEmpty());
        QVERIFY(!mask->accessibleDescription().isEmpty());
        emit backend->capabilitiesChanged();
        QVERIFY(!apply->toolTip().isEmpty());
        emit apply->clicked();
        QVERIFY(backend->commands.isEmpty());
        address->setText(QStringLiteral("192.0.2.2"));
        mask->setText(QStringLiteral("255.255.255.0"));
        QVERIFY(apply->isEnabled());
        QVERIFY(address->toolTip().isEmpty());
        apply->click();
        QCOMPARE(backend->commands, QStringList{"radio static_net_params ip=192.0.2.2 gateway=192.0.2.1 netmask=255.255.255.0"});
        QVERIFY(dropped.isEmpty());
    }

    void malformedTimingsRestoreObservedValues()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupDialog dialog(&radio);
        dialog.selectTab(QStringLiteral("Transmit"));
        QLineEdit* delay = editNextTo(dialog, QStringLiteral("TX Delay:"));
        QLineEdit* timeout = editNextTo(dialog, QStringLiteral("Timeout (sec):"));
        QVERIFY(delay && timeout);
        const QString observedDelay = delay->text();
        const QString observedTimeout = timeout->text();
        for (const QString& input : {QStringLiteral("oops"), QStringLiteral("999999999999999999999")}) {
            delay->setText(input);
            emit delay->editingFinished();
            QCOMPARE(delay->text(), observedDelay);
        }
        timeout->setText(QStringLiteral("2147484"));
        emit timeout->editingFinished();
        QCOMPARE(timeout->text(), observedTimeout);
        QVERIFY(backend->commands.isEmpty());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-operations"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    RadioSetupOperationsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_operations_test.moc"
