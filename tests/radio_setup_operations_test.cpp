#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "gui/RadioSetupDialog.h"
#include "gui/TxBandDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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
        QCOMPARE(backend->commands, QStringList({"radio set cal_freq=10.000000",
                                                "radio set freq_error_ppb=0", "radio pll_start"}));
        QVERIFY(!start->isEnabled());
        backend->replies.back()({RadioSetupReply::Kind::Accepted});
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
        backend->replies.back()({RadioSetupReply::Kind::Failed, 9, QStringLiteral("rejected")});
        QVERIFY(start->isEnabled());
        start->click();
        const RadioSetupCompletion trailing = backend->replies.back();
        trailing({RadioSetupReply::Kind::Accepted});
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
        dialog.reset();
        closed({RadioSetupReply::Kind::Accepted});
        QCoreApplication::processEvents();
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

    void bandDialogUsesTypedRequestsAndRejectsInvalidNumbers()
    {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::band(radio, 9);
        TxBandDialog dialog(&radio);
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
        editors[1]->setText(QStringLiteral("101"));
        emit editors[1]->editingFinished();
        QVERIFY(backend->commands.isEmpty());
        const QList<QCheckBox*> toggles = dialog.findChildren<QCheckBox*>();
        QCOMPARE(toggles.size(), 8);
        toggles[0]->setChecked(true);
        QCOMPARE(backend->commands, QStringList{"transmit bandset 9 inhibit=1"});
        backend = installRadioSetupBackend(radio);
        RadioSetupControlTestAccess::band(radio, 9);
        editors[0]->setText(QStringLiteral("55"));
        emit editors[0]->editingFinished();
        QVERIFY(backend->commands.isEmpty());
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-operations"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupOperationsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_operations_test.moc"
