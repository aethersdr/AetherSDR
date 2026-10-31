#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/NetScheduleStore.h"
#include "gui/NetSchedulerDialog.h"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableWidget>
#include <QStringList>
#include <QTimer>
#include <QtTest>

using namespace AetherSDR;

namespace {

QPushButton* buttonByText(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

QComboBox* modeCombo(QWidget& parent)
{
    for (QComboBox* combo : parent.findChildren<QComboBox*>()) {
        if (combo->findText(QStringLiteral("USB")) >= 0
            && combo->findText(QStringLiteral("LSB")) >= 0) {
            return combo;
        }
    }
    return nullptr;
}

} // namespace

class NetSchedulerModesTest : public QObject
{
    Q_OBJECT

private slots:
    void modernChoicesWithoutCapture()
    {
        NetSchedulerDialog dialog({}, {});
        QStringList choices;
        QString selected;
        QTimer::singleShot(0, &dialog, [&] {
            auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!editor) {
                return;
            }
            if (QComboBox* combo = modeCombo(*editor)) {
                selected = combo->currentText();
                for (int index = 0; index < combo->count(); ++index) {
                    choices.append(combo->itemText(index));
                }
            }
            editor->reject();
        });
        QPushButton* add = buttonByText(dialog, QStringLiteral("Add…"));
        QVERIFY(add);
        add->click();
        QCOMPARE(selected, QStringLiteral("FM"));
        for (const QString& mode : {QStringLiteral("FDVU"), QStringLiteral("FDVL"),
                                    QStringLiteral("FDV")}) {
            QVERIFY(choices.contains(mode));
        }
        QVERIFY(!choices.contains(QStringLiteral("CUSTOM-WF")));
        QVERIFY(dialog.entries().isEmpty());
    }

    void presetModesRoundTrip_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<int>("source");
        QTest::addColumn<QString>("expected");
        for (const char* mode : {"FDVU", "FDVL", "FDV", "NT", "CUSTOM-WF", "fdvu"}) {
            // 0: Add's automatic VFO seed; 1: explicit Capture current VFO;
            // 2: reopen a serialized preset without capturing the live VFO.
            for (int source = 0; source < 3; ++source) {
                const QByteArray name = QByteArray(mode) + '-' + QByteArray::number(source);
                QTest::newRow(name.constData()) << QString::fromLatin1(mode) << source
                                              << (QString(mode) == "fdvu" ? QStringLiteral("FDVU")
                                                                        : QString::fromLatin1(mode));
            }
        }
        for (int source = 0; source < 3; ++source) {
            const QByteArray suffix = '-' + QByteArray::number(source);
            QTest::newRow(("padded-fdvl" + suffix).constData())
                << QStringLiteral(" fdvl ") << source << QStringLiteral("FDVL");
            QTest::newRow(("control-fdvu" + suffix).constData())
                << QStringLiteral("F\nD\rV\tU") << source << QStringLiteral("FDVU");
            QTest::newRow(("custom-lowercase" + suffix).constData())
                << QStringLiteral("custom-wf") << source << QStringLiteral("CUSTOM-WF");
        }
    }

    void presetModesRoundTrip()
    {
        QFETCH(QString, mode);
        QFETCH(int, source);
        QFETCH(QString, expected);

        MemoryEntry captured;
        captured.freq = 14.236;
        captured.mode = mode;
        captured.rxFilterLow = mode == QStringLiteral("FDVL") ? -3000 : 95;
        captured.rxFilterHigh = mode == QStringLiteral("FDVL") ? -95 : 3000;

        QList<NetEntry> entries;
        if (source != 0) {
            NetEntry entry;
            entry.id = QStringLiteral("freedv-net");
            entry.name = QStringLiteral("FreeDV net");
            entry.preset = captured;
            if (source == 1) {
                entry.preset.mode = QStringLiteral("FM");
            }
            const NetScheduleStore::ParseResult loaded =
                NetScheduleStore::parse(NetScheduleStore::serialize({entry}));
            QVERIFY(loaded.ok());
            QCOMPARE(loaded.nets.size(), 1);
            entries = loaded.nets;
        }

        int captures = 0;
        NetSchedulerDialog dialog(entries, [&] {
            ++captures;
            return captured;
        });
        int changes = 0;
        QList<NetEntry> saved;
        connect(&dialog, &NetSchedulerDialog::entriesChanged, this,
                [&](const QList<NetEntry>& result) {
            ++changes;
            saved = result;
        });

        QTableWidget* table = dialog.findChild<QTableWidget*>();
        QVERIFY(table);
        if (source != 0) {
            table->setCurrentCell(0, 1);
        }
        QPushButton* open = buttonByText(dialog, source == 0 ? QStringLiteral("Add…")
                                                           : QStringLiteral("Edit…"));
        QVERIFY(open);

        bool openedEditor = false;
        bool capturedByButton = false;
        QString displayedMode;
        QTimer::singleShot(0, &dialog, [&] {
            auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!editor) {
                return;
            }
            openedEditor = true;
            if (source == 1) {
                QPushButton* capture = buttonByText(*editor, QStringLiteral("Capture current VFO"));
                if (capture) {
                    capture->click();
                    capturedByButton = true;
                }
            }
            if (QComboBox* combo = modeCombo(*editor)) {
                displayedMode = combo->currentText();
            }
            auto* buttons = editor->findChild<QDialogButtonBox*>();
            if (buttons && buttons->button(QDialogButtonBox::Ok)) {
                buttons->button(QDialogButtonBox::Ok)->click();
            } else {
                editor->reject();
            }
        });
        open->click();

        QVERIFY(openedEditor);
        QCOMPARE(capturedByButton, source == 1);
        QCOMPARE(captures, source == 2 ? 0 : 1);
        QCOMPARE(displayedMode, expected);
        QCOMPARE(changes, 1);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved.first().preset.mode, expected);
        QCOMPARE(saved.first().preset.freq, captured.freq);
        QCOMPARE(saved.first().preset.rxFilterLow, captured.rxFilterLow);
        QCOMPARE(saved.first().preset.rxFilterHigh, captured.rxFilterHigh);

        const NetScheduleStore::ParseResult reloaded =
            NetScheduleStore::parse(NetScheduleStore::serialize(saved));
        QVERIFY(reloaded.ok());
        QCOMPARE(reloaded.nets.size(), 1);
        QCOMPARE(reloaded.nets.first().preset.mode, expected);

        NetSchedulerDialog reopened(reloaded.nets, {});
        int tunes = 0;
        NetEntry tuned;
        connect(&reopened, &NetSchedulerDialog::tuneRequested, this,
                [&](const NetEntry& entry) {
            ++tunes;
            tuned = entry;
        });
        QTableWidget* reopenedTable = reopened.findChild<QTableWidget*>();
        QVERIFY(reopenedTable);
        reopenedTable->setCurrentCell(0, 1);
        QPushButton* tune = buttonByText(reopened, QStringLiteral("Tune Now"));
        QVERIFY(tune);
        QVERIFY(tune->isEnabled());
        tune->click();
        QCOMPARE(tunes, 1);
        QCOMPARE(tuned.preset.mode, expected);
        QCOMPARE(tuned.preset.freq, captured.freq);
        QCOMPARE(tuned.preset.rxFilterLow, captured.rxFilterLow);
        QCOMPARE(tuned.preset.rxFilterHigh, captured.rxFilterHigh);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("net-scheduler-modes"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    QTimer watchdog;
    QObject::connect(&watchdog, &QTimer::timeout, &app, [] {
        qFatal("Net scheduler mode test failed to leave its editor loop");
    });
    watchdog.start(15000);
    NetSchedulerModesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "net_scheduler_modes_test.moc"
