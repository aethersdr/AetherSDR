// #5637 §3 — Radio Setup → Transmit's `Max Power:` field.
//
// The field shows TransmitModel::maxPowerLevel() and was labelled `%` on every
// radio. On a backend that declares RadioCapabilities::txPowerBands that value
// is set by RadioModel::refreshTxPowerLimit from the band's rated WATTS — the
// Hermes-Lite 2's 5 W power class, which the forward-power gauges already read
// as watts. On an HL2 the dialog therefore said "5 %", accepted an edit,
// dropped the write (no command plane), and read 5 back on the next open: the
// "Max Power reverts from 100% to 5%" in the report.
//
// Pinned here, each with its control:
//   1. a watt ceiling from the band table is labelled W, not %;
//   2. with no command plane the field is read-only and says why on an
//      accessible channel (a tooltip alone never reaches a screen reader);
//   3. finishing an edit there raises no commandDropped — nothing is offered
//      that cannot be sent;
//   4. before the backend has reported a rating, no number and no W are shown
//      — TransmitModel's compiled-in 100 is not a rating;
//   5. the Flex path is unchanged from main: editable, and an edit still
//      reaches RadioModel::sendCommand. Its unit label is deliberately NOT
//      asserted — which unit Flex's max_power_level carries is an open
//      question this change does not answer.
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
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QStringList>
#include <QtTest>

using namespace AetherSDR;

namespace {

QStringList* g_logSink = nullptr;

void captureLogHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (g_logSink) {
        *g_logSink << msg;
    }
}

// RadioModel has no outbound-command signal; sendCommand() logs each command
// at debug on aether.protocol (the same capture tci_server_review_test uses).
class ScopedCommandLog
{
public:
    ScopedCommandLog()
    {
        g_logSink = &m_lines;
        m_previous = qInstallMessageHandler(captureLogHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("aether.protocol.debug=true"));
    }
    ~ScopedCommandLog()
    {
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(m_previous);
        g_logSink = nullptr;
    }
    ScopedCommandLog(const ScopedCommandLog&) = delete;
    ScopedCommandLog& operator=(const ScopedCommandLog&) = delete;

    bool contains(const QString& fragment) const
    {
        for (const QString& line : m_lines) {
            if (line.contains(fragment)) {
                return true;
            }
        }
        return false;
    }

private:
    QStringList      m_lines;
    QtMessageHandler m_previous{nullptr};
};

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

    // The state the review found (#6015): an HL2 backend built but not yet
    // connected, so refreshTxPowerLimit() has never run and maxPowerLevel() is
    // still TransmitModel's compiled-in 100. That is not the radio's rating,
    // and "100 W" described as "the radio's rated output" is wrong for a 5 W
    // radio.
    void hl2UnreportedRatingShowsNoPhantomWatts()
    {
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("hl2")));
        QVERIFY2(!model.backendCapabilities().txPowerBands.isEmpty(),
                 "HL2 declares its power class as a band table");
        QVERIFY2(!model.transmitModel().haveMaxPowerLevel(),
                 "premise: no rating has been reported");

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        const MaxPowerField field = findMaxPowerField(dialog);
        QVERIFY2(field.edit, "Max Power edit found");
        QVERIFY2(field.unit, "Max Power unit label found");

        QVERIFY2(field.edit->text().isEmpty(),
                 qPrintable(QStringLiteral("no phantom rating, got \"%1\"")
                                .arg(field.edit->text())));
        QVERIFY2(field.unit->text() != QStringLiteral("W"),
                 "no W before any watts have been reported");
        QVERIFY2(field.edit->isReadOnly(), "still no command plane");
        QVERIFY2(!field.edit->accessibleDescription().startsWith(
                     QStringLiteral("The radio's rated output")),
                 "does not describe an unreported value as the rated output");
        QVERIFY2(!field.edit->accessibleDescription().isEmpty(),
                 "the reason still reaches a screen reader");
    }

    // POSITIVE CONTROL: the Flex path is unchanged from main — the field is
    // editable and an edit reaches sendCommand as `transmit set
    // max_power_level=` — so the assertions above cannot be passed by a field
    // made read-only on every radio. The unit label is NOT asserted, and the
    // value typed is inside every reading's range: which unit Flex reports is
    // not settled here, and this test must not freeze either answer.
    void flexPathUnchangedFromMain()
    {
        RadioModel model;  // a bare model is on the Flex backend
        QVERIFY2(model.hasCommandPlane(), "Flex has a command plane");
        QVERIFY(model.backendCapabilities().txPowerBands.isEmpty());

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        const MaxPowerField field = findMaxPowerField(dialog);
        QVERIFY2(field.edit, "Max Power edit found");

        QCOMPARE(field.edit->text(),
                 QString::number(model.transmitModel().maxPowerLevel()));
        QVERIFY2(!field.edit->isReadOnly(), "Flex max_power_level stays writable");

        ScopedCommandLog log;
        field.edit->setText(QStringLiteral("80"));
        emit field.edit->editingFinished();
        QVERIFY2(log.contains(QStringLiteral("transmit set max_power_level=80")),
                 "the edit still reaches RadioModel::sendCommand");
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
