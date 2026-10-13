#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "core/MemoryCsvCompat.h"
#include "gui/ProfileManagerDialog.h"
#include "gui/MemoryDialog.h"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <cstdio>

using namespace AetherSDR;

namespace AetherSDR {
class StoredControlTestAccess {
public:
    static void memory(RadioModel& model, const MemoryDelta& delta) { model.applyMemoryChanges(delta); }
    static void profile(RadioModel& model, const ProfileDelta& delta) { model.applyProfileChanges(delta); }
};
class StoredDialogsTestAccess {
public:
    static QTableWidget* table(MemoryDialog& dialog) { return dialog.m_table; }
    static void populate(MemoryDialog& dialog) { dialog.populateTable(); }
    static void submit(MemoryDialog& dialog, int row, int column) { dialog.submitCellEdit(row, column); }
    static void importRows(MemoryDialog& dialog, const MemoryCsvParseResult& parsed)
    { dialog.startImport(parsed, QStringLiteral("test.csv"), dialog.m_model->controlSession(), dialog.m_model->usesLocalMemoryBank()); }
};
}

namespace {
int failures{0};
void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void profileDialog()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    ProfileManagerDialog dialog(&model);
    QLineEdit* edit = dialog.findChild<QLineEdit*>("profileNameEdit_global");
    QPushButton* save = dialog.findChild<QPushButton*>("profileSaveBtn_global");
    QLabel* status = dialog.findChild<QLabel*>("profileStatus_global");
    check(edit && save && status, "profile controls retain their identities");
    if (!edit || !save || !status) { return; }
    edit->setText("Test profile"); save->click();
    check(backend->commands == QStringList{"profile global save \"Test profile\""}, "dialog save goes through typed seam");
    const RadioControlCompletion first = backend->replies.at(0);
    edit->setText("Second profile"); save->click();
    const RadioControlCompletion second = backend->replies.at(1);
    first({RadioControlReply::Kind::Accepted, 0, {}});
    check(edit->text() == "Second profile" && status->text().contains("Saving"), "superseded save cannot clear new input or status");
    second({RadioControlReply::Kind::Failed, 9, "refused"});
    check(edit->text() == "Second profile" && status->text().contains("refused"), "failed save retains target");
    edit->setText("Bad\"name"); save->click();
    check(backend->commands.size() == 2 && edit->text() == "Bad\"name", "unsafe name neither writes nor clears input");
    edit->setText("Final profile"); save->click();
    const RadioControlCompletion pending = backend->replies.back();
    RadioSetupControlTestAccess::replaceConnectionGeneration(model);
    pending({RadioControlReply::Kind::Accepted, 0, {}});
    check(edit->text() == "Final profile" && !status->text().contains("Saved profile"), "stale save never reports success on replacement session");
}
void memoryDialog()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    MemoryDelta delta; delta.index = 7; delta.name = "Before"; delta.mode = "FM";
    delta.freq = 146.52; delta.group = "Test";
    StoredControlTestAccess::memory(model, delta);
    MemoryDialog dialog(&model);
    StoredDialogsTestAccess::populate(dialog);
    QTableWidget* table = StoredDialogsTestAccess::table(dialog);
    check(table && table->rowCount() == 1, "memory observation populates dialog");
    if (!table || table->rowCount() != 1) { return; }
    // Block itemChanged while constructing the edit, then use the actual submission path.
    table->blockSignals(true); table->item(0, 3)->setText("After name"); table->blockSignals(false);
    StoredDialogsTestAccess::submit(dialog, 0, 3);
    check(backend->commands == QStringList{QStringLiteral("memory set 7 name=After") + QChar(0x7f) + QStringLiteral("name")},
          "single-cell edit sends only its typed field");
    const RadioControlCompletion edit = backend->replies.at(0);
    edit({RadioControlReply::Kind::Accepted, 0, {}});
    check(model.memories().value(7).name == "After name", "accepted edit reconciles cache");
    table->blockSignals(true); table->item(0, 2)->setText("nan"); table->blockSignals(false);
    StoredDialogsTestAccess::submit(dialog, 0, 2);
    check(backend->commands.size() == 1 && model.memories().value(7).freq == 146.52,
          "malformed cell cannot reach transport or change memory");
}
void stoppedImport()
{
    RadioModel model;
    RadioSetupTestBackend* backend = installRadioSetupBackend(model);
    MemoryDialog dialog(&model);
    QList<MemoryCsvRecord> records;
    for (const QString& name : {QStringLiteral("First row"), QStringLiteral("Second row"), QStringLiteral("Third row")}) {
        MemoryEntry entry; entry.name = name; entry.freq = 14.1; entry.mode = "USB"; entry.step = 1000;
        records.append(MemoryCsvCompat::fromMemoryEntry(entry));
    }
    const QByteArray bytes = MemoryCsvCompat::serialize(records);
    const MemoryCsvParseResult parsed = MemoryCsvCompat::parse(bytes);
    if (parsed.records.size() != 3 || !parsed.errors.isEmpty()) { check(false, "three synthetic CSV records parse"); return; }
    QString summary;
    QString details;
    QTimer driver;
    QObject::connect(&driver, &QTimer::timeout, &dialog, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QMessageBox* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible()) {
                summary = box->informativeText(); details = box->detailedText(); box->accept();
            }
        }
    });
    driver.start(20);
    QTimer::singleShot(10000, &dialog, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (QDialog* modal = qobject_cast<QDialog*>(widget); modal && modal->isModal()) { modal->reject(); }
        }
    });
    StoredDialogsTestAccess::importRows(dialog, parsed);
    if (backend->commands.size() != 1 || backend->replies.empty()) { check(false, "import starts first typed create"); return; }
    const RadioControlCompletion created = backend->replies.at(0);
    created({RadioControlReply::Kind::Accepted, 0, "9"});
    if (backend->replies.size() != 2) { check(false, "import populates first created slot"); return; }
    const RadioControlCompletion populated = backend->replies.at(1);
    populated({RadioControlReply::Kind::Accepted, 0, {}});
    RadioSetupControlTestAccess::replaceConnectionGeneration(model);
    QTest::qWait(100);
    check(backend->commands.size() == 2, "queued import never writes to a replacement session");
    if (!summary.startsWith("2 rows were skipped") || !details.contains("Second row") || !details.contains("Third row")) {
        std::fprintf(stderr, "Import summary: %s\nImport details: %s\n", qPrintable(summary), qPrintable(details));
    }
    check(summary.startsWith("2 rows were skipped") && details.contains("Second row") && details.contains("Third row"),
          "queued import reports every unattempted row after session replacement");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("stored-control-dialogs-test"));
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!profile.isValid()) { return 2; }
    AppSettings::instance().load();
    profileDialog(); memoryDialog(); stoppedImport();
    return failures == 0 ? 0 : 1;
}
