// #5637 §3 — Radio Setup → Transmit's `Max Power:` field.
//
// The field shows TransmitModel::maxPowerLevel() and was labelled `%` on every
// radio. That number has two producers: a Flex reports `max_power_level` in
// its transmit status (a percent, and a level the operator may write back with
// `transmit set max_power_level=`), while a backend that declares
// RadioCapabilities::txPowerBands has it set by RadioModel::refreshTxPowerLimit
// from the band's rated WATTS — the Hermes-Lite 2's 5 W power class, which the
// forward-power gauges already read as watts. On an HL2 the dialog therefore
// said "5 %", accepted an edit, dropped the write (no command plane), and read
// 5 back on the next open: the "Max Power reverts from 100% to 5%" in the
// report.
//
// Pinned here, each with its control:
//   1. a watt ceiling from the band table is labelled W, not %;
//   2. with no command plane the field is read-only and says why on an
//      accessible channel (a tooltip alone never reaches a screen reader);
//   3. finishing an edit there raises no commandDropped — nothing is offered
//      that cannot be sent;
//   4. the Flex field is unchanged: `%`, editable.
//
// No hardware and no transport: the HL2 backend is the real one, built through
// rebuildBackendForTest() and never connected.

#include "TestSettingsProfile.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;

namespace {

struct MaxPowerField {
    QLineEdit* edit{nullptr};
    QLabel* unit{nullptr};
};

// Found from the caption the operator reads rather than from an object name, so
// the same lookup works on the code before this change and after it — which is
// what lets the assertions below fail on the old dialog instead of failing to
// find anything.
MaxPowerField findMaxPowerField(QWidget& dialog)
{
    MaxPowerField out;
    for (QGridLayout* grid : dialog.findChildren<QGridLayout*>()) {
        for (int i = 0; i < grid->count(); ++i) {
            auto* caption = qobject_cast<QLabel*>(grid->itemAt(i)->widget());
            if (!caption || caption->text() != QStringLiteral("Max Power:")) {
                continue;
            }
            int row = 0, col = 0, rowSpan = 0, colSpan = 0;
            grid->getItemPosition(i, &row, &col, &rowSpan, &colSpan);
            QLayoutItem* valueItem = grid->itemAtPosition(row, col + 1);
            QLayout* rowLayout = valueItem ? valueItem->layout() : nullptr;
            if (!rowLayout || rowLayout->count() < 2) {
                return out;
            }
            out.edit = qobject_cast<QLineEdit*>(rowLayout->itemAt(0)->widget());
            out.unit = qobject_cast<QLabel*>(rowLayout->itemAt(1)->widget());
            return out;
        }
    }
    return out;
}

} // namespace

class RadioSetupMaxPowerFieldTest : public QObject {
    Q_OBJECT
private slots:

    void hl2RatedWattsReadAsWattsAndCannotBeEdited()
    {
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("hl2")));
        // The premises, asserted rather than assumed.
        QVERIFY2(!model.hasCommandPlane(), "HL2 has no command plane");
        QVERIFY2(!model.backendCapabilities().txPowerBands.isEmpty(),
                 "HL2 declares its power class as a band table");
        // What refreshTxPowerLimit() writes on a connected HL2 (kHl2RatedOutputWatts).
        model.transmitModel().setMaxPowerLevel(5);

        QSignalSpy dropped(&model, &RadioModel::commandDropped);

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        const MaxPowerField field = findMaxPowerField(dialog);
        QVERIFY2(field.edit, "Max Power edit found");
        QVERIFY2(field.unit, "Max Power unit label found");

        QCOMPARE(field.edit->text(), QStringLiteral("5"));
        QCOMPARE(field.unit->text(), QStringLiteral("W"));
        QVERIFY2(field.edit->isReadOnly(),
                 "no command plane: the rated output is display-only");
        QVERIFY2(!field.edit->accessibleDescription().isEmpty(),
                 "the reason reaches a screen reader, not only a tooltip");

        // An operator who types into it and presses Enter.
        field.edit->setText(QStringLiteral("100"));
        emit field.edit->editingFinished();
        QCOMPARE(dropped.count(), 0);
    }

    // POSITIVE CONTROL: the Flex field keeps its percent and stays editable, so
    // the assertions above cannot be passed by a field made read-only and
    // relabelled on every radio.
    void flexLevelStaysAnEditablePercent()
    {
        RadioModel model;  // a bare model is on the Flex backend
        QVERIFY2(model.hasCommandPlane(), "Flex has a command plane");
        QVERIFY(model.backendCapabilities().txPowerBands.isEmpty());

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        const MaxPowerField field = findMaxPowerField(dialog);
        QVERIFY2(field.edit, "Max Power edit found");
        QVERIFY2(field.unit, "Max Power unit label found");

        QCOMPARE(field.unit->text(), QStringLiteral("%"));
        QVERIFY2(!field.edit->isReadOnly(), "Flex max_power_level is writable");
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-max-power-field"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupMaxPowerFieldTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_max_power_field_test.moc"
